// SPDX-License-Identifier: MIT
/*
 * gc2607d - own userspace pipeline for the GalaxyCore GC2607 on Intel IPU6.
 *
 * SGRBG10 raw -> black level -> bilinear debayer -> AWB -> CCM -> sRGB gamma
 * -> denoise -> YUYV -> v4l2loopback, with AE/AWB feedback to the sensor.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <omp.h>
#include <signal.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/videodev2.h>

#define WIDTH        1920
#define HEIGHT       1080
#define NBUF         4
#define BLACK_LEVEL  64.0f
#define GAMMA_LUT_N  4096

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

struct capbuf { void *start; size_t len; };

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------- pipeline discovery ---------- */

static char *read_cmd(const char *cmd)
{
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(p); return NULL; }
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, p)) > 0) {
        len += n;
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) { pclose(p); return NULL; } }
    }
    buf[len] = 0;
    pclose(p);
    return buf;
}

static int discover(char *node, size_t nlen, int *port_out)
{
    char *t = read_cmd("media-ctl -d /dev/media0 -p 2>/dev/null");
    if (!t) return -1;

    int port = -1, capidx = -1;
    char *save = NULL;
    for (char *blk = strtok_r(t, "\n", &save); blk; blk = strtok_r(NULL, "\n", &save)) {
        (void)blk;
    }
    free(t);
    t = read_cmd("media-ctl -d /dev/media0 -p 2>/dev/null");
    if (!t) return -1;

    /* split into entity blocks */
    char *blocks[512];
    int nb = 0;
    char *p = t;
    while (p && *p && nb < 512) {
        char *next = strstr(p, "\n- entity ");
        blocks[nb++] = p;
        if (!next) break;
        *next = 0;
        p = next + 1;
    }

    for (int i = 0; i < nb; i++) {
        int pn;
        if (sscanf(blocks[i], "- entity %*d: Intel IPU6 CSI2 %d", &pn) == 1 &&
            strstr(blocks[i], "gc2607")) {
            port = pn;
            break;
        }
    }
    if (port < 0) { free(t); return -1; }

    for (int i = 0; i < nb; i++) {
        int pn, ci;
        if (sscanf(blocks[i], "- entity %*d: Intel IPU6 CSI2 %d", &pn) == 1 && pn == port) {
            char *l = strstr(blocks[i], "-> \"Intel IPU6 ISYS Capture ");
            if (l && sscanf(l, "-> \"Intel IPU6 ISYS Capture %d\"", &ci) == 1)
                capidx = ci;
        }
    }
    if (capidx < 0) { free(t); return -1; }

    for (int i = 0; i < nb; i++) {
        int ci;
        if (sscanf(blocks[i], "- entity %*d: Intel IPU6 ISYS Capture %d", &ci) == 1 && ci == capidx) {
            char *d = strstr(blocks[i], "/dev/video");
            if (d) sscanf(d, "%*[^ \n]"); /* keep */
            if (d) {
                size_t k = 0;
                while (d[k] && d[k] != ' ' && d[k] != '\n' && k < nlen - 1) { node[k] = d[k]; k++; }
                node[k] = 0;
            }
        }
    }
    free(t);
    if (node[0] == 0) return -1;
    *port_out = port;
    return 0;
}

static void setup_pipeline(int port, const char *node)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "media-ctl -d /dev/media0 -V '\"Intel IPU6 CSI2 %d\":0 [fmt:SGRBG10_1X10/%dx%d]' >/dev/null 2>&1", port, WIDTH, HEIGHT);
    if (system(cmd)) {}
    snprintf(cmd, sizeof(cmd), "media-ctl -d /dev/media0 -V '\"Intel IPU6 CSI2 %d\":1 [fmt:SGRBG10_1X10/%dx%d]' >/dev/null 2>&1", port, WIDTH, HEIGHT);
    if (system(cmd)) {}
    snprintf(cmd, sizeof(cmd), "v4l2-ctl -d %s --set-fmt-video=width=%d,height=%d,pixelformat=BA10 >/dev/null 2>&1", node, WIDTH, HEIGHT);
    if (system(cmd)) {}
    int ci = atoi(node + strlen("/dev/video"));
    snprintf(cmd, sizeof(cmd), "media-ctl -d /dev/media0 -l '\"Intel IPU6 CSI2 %d\":1 -> \"Intel IPU6 ISYS Capture %d\":0[1]' >/dev/null 2>&1", port, ci);
    if (system(cmd)) {}
}


/* find the gc2607 V4L2 subdev node (numbering changes across boots) */
static int find_subdev(char *out, size_t n)
{
    DIR *d = opendir("/sys/class/video4linux");
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "v4l-subdev", 10)) continue;
        char path[512], name[300];
        snprintf(path, sizeof(path), "/sys/class/video4linux/%s/name", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int ok = fgets(name, sizeof(name), f) && strstr(name, "gc2607");
        fclose(f);
        if (ok) {
            snprintf(out, n, "/dev/%s", e->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* ---------- v4l2 ---------- */


static int cap_open(const char *node, struct capbuf bufs[NBUF])
{
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open capture"); return -1; }

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = WIDTH;
    f.fmt.pix.height = HEIGHT;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_SGRBG10;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("S_FMT"); { close(fd); return -1; } }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NBUF;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); { close(fd); return -1; } }

    for (unsigned i = 0; i < NBUF; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); { close(fd); return -1; } }
        bufs[i].len = b.length;
        bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (bufs[i].start == MAP_FAILED) { perror("mmap"); { close(fd); return -1; } }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); { close(fd); return -1; } }
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &t) < 0) { perror("STREAMON"); { close(fd); return -1; } }
    return fd;
}

static int out_open(const char *node)
{
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open loopback"); return -1; }
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f.fmt.pix.width = WIDTH;
    f.fmt.pix.height = HEIGHT;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) {
        /* retry letting the driver fill in the geometry */
        memset(&f, 0, sizeof(f));
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        f.fmt.pix.width = WIDTH;
        f.fmt.pix.height = HEIGHT;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) { perror("loopback S_FMT"); return -1; }
    }
    return fd;
}

/* ---------- image pipeline ---------- */

struct params {
    float awb[3];        /* r, g, b digital gains */
    float ccm[9];
    uint8_t gamma[GAMMA_LUT_N];
    float black;
    int denoise;
};

static void params_init(struct params *p)
{
    p->awb[0] = p->awb[1] = p->awb[2] = 1.0f;
    /* conservative identity-preserving colour matrix (rows sum to 1) */
    static const float m[9] = {
         1.45f, -0.35f, -0.10f,
        -0.20f,  1.40f, -0.20f,
         0.00f, -0.30f,  1.30f,
    };
    memcpy(p->ccm, m, sizeof(m));
    p->black = BLACK_LEVEL;
    p->denoise = 1;
    for (int i = 0; i < GAMMA_LUT_N; i++) {
        float x = (float)i / (GAMMA_LUT_N - 1);
        float y = x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
        p->gamma[i] = (uint8_t)(y * 255.0f + 0.5f);
    }
}

static inline uint8_t lin_to_srgb(const struct params *p, float v)
{
    if (!(v > 0.0f)) return 0;
    int i = (int)(v * (GAMMA_LUT_N - 1));
    if (i >= GAMMA_LUT_N) i = GAMMA_LUT_N - 1;
    return p->gamma[i];
}

/* bilinear debayer for GRBG, row y; assumes rows y-1..y+1 available */
static inline void debayer_px(const uint16_t *r0, const uint16_t *r1, const uint16_t *r2,
                              int x, int y, float out[3])
{
    const int odd_row = y & 1, odd_col = x & 1;
    float c = r1[x];
    float horiz = (r1[x > 0 ? x - 1 : x] + r1[x + 1 < WIDTH ? x + 1 : x]) * 0.5f;
    float vert = (r0[x] + r2[x]) * 0.5f;
    float diag = (r0[x > 0 ? x - 1 : x] + r0[x + 1 < WIDTH ? x + 1 : x] +
                  r2[x > 0 ? x - 1 : x] + r2[x + 1 < WIDTH ? x + 1 : x]) * 0.25f;

    if (!odd_row && odd_col) {          /* R */
        out[0] = c; out[1] = (horiz + vert) * 0.5f; out[2] = diag;
    } else if (odd_row && !odd_col) {   /* B */
        out[2] = c; out[1] = (horiz + vert) * 0.5f; out[0] = diag;
    } else {                            /* G */
        out[1] = c;
        out[0] = odd_col ? vert : horiz;
        out[2] = odd_col ? horiz : vert;
    }
}

/*
 * Convert one frame of raw 16-bit container data to YUYV.
 * stats: mean of R,G,B (post black level) and mean luma, over a subsample.
 */
static void process_frame(const uint16_t *raw, uint8_t *yuyv,
                          const struct params *p, float stats[6])
{
    static const float sr = 0.299f, sg = 0.587f, sb = 0.114f;

    #pragma omp parallel
    {
        float lr = 0, lg = 0, lb = 0;
        float ly = 0;
        long n = 0, ny = 0;

        #pragma omp for schedule(static)
        for (int y = 0; y < HEIGHT; y++) {
            const uint16_t *r0 = raw + ((y > 0 ? y - 1 : 0) * WIDTH);
            const uint16_t *r1 = raw + (y * WIDTH);
            const uint16_t *r2 = raw + ((y + 1 < HEIGHT ? y + 1 : HEIGHT - 1) * WIDTH);
            uint8_t *dst = yuyv + (size_t)y * WIDTH * 2;

            for (int x = 0; x < WIDTH; x += 2) {
                float rgb[2][3];
                float yv[2];
                for (int k = 0; k < 2; k++) {
                    float c[3];
                    debayer_px(r0, r1, r2, x + k, y, c);
                    for (int ch = 0; ch < 3; ch++) {
                        float v = (c[ch] - p->black) * (1.0f / 1023.0f) * p->awb[ch];
                        if (v < 0) v = 0;
                        c[ch] = v;
                    }
                    float o[3];
                    for (int r = 0; r < 3; r++)
                        o[r] = p->ccm[r * 3] * c[0] + p->ccm[r * 3 + 1] * c[1] + p->ccm[r * 3 + 2] * c[2];
                    rgb[k][0] = o[0]; rgb[k][1] = o[1]; rgb[k][2] = o[2];
                    yv[k] = sr * o[0] + sg * o[1] + sb * o[2];
                    if ((y & 7) == 0 && ((x + k) & 15) == 0) {
                        lr += c[0]; lg += c[1]; lb += c[2]; n++;
                    }
                }
                uint8_t Y0 = lin_to_srgb(p, yv[0] >= 0 ? yv[0] : 0);
                uint8_t Y1 = lin_to_srgb(p, yv[1] >= 0 ? yv[1] : 0);
                float gr = 0.5f * ((float)lin_to_srgb(p, rgb[0][0] > 0 ? rgb[0][0] : 0) +
                                   (float)lin_to_srgb(p, rgb[1][0] > 0 ? rgb[1][0] : 0)) / 255.0f;
                float gg = 0.5f * ((float)lin_to_srgb(p, rgb[0][1] > 0 ? rgb[0][1] : 0) +
                                   (float)lin_to_srgb(p, rgb[1][1] > 0 ? rgb[1][1] : 0)) / 255.0f;
                float gb = 0.5f * ((float)lin_to_srgb(p, rgb[0][2] > 0 ? rgb[0][2] : 0) +
                                   (float)lin_to_srgb(p, rgb[1][2] > 0 ? rgb[1][2] : 0)) / 255.0f;
                float u = -0.169f * gr - 0.331f * gg + 0.5f * gb;
                float v = 0.5f * gr - 0.419f * gg - 0.081f * gb;
                u = 0.5f + u; v = 0.5f + v;
                int U = (int)(u * 255.0f); if (U < 0) U = 0; if (U > 255) U = 255;
                int V = (int)(v * 255.0f); if (V < 0) V = 0; if (V > 255) V = 255;
                dst[0] = Y0; dst[1] = (uint8_t)U; dst[2] = Y1; dst[3] = (uint8_t)V;
                dst += 4;
                ly += (Y0 + Y1) * 0.5f;
                ny += 1;
            }
        }
        #pragma omp critical
        {
            stats[0] += lr; stats[1] += lg; stats[2] += lb;
            stats[3] += (float)n;
            stats[4] += ly; stats[5] += (float)ny;
        }
    }
}

static void denoise_y(uint8_t *yuyv)
{
    #pragma omp parallel for schedule(static)
    for (int y = 1; y < HEIGHT - 1; y++) {
        uint8_t *row = yuyv + (size_t)y * WIDTH * 2;
        uint8_t *up = yuyv + (size_t)(y - 1) * WIDTH * 2;
        uint8_t *dn = yuyv + (size_t)(y + 1) * WIDTH * 2;
        for (int x = 0; x < WIDTH; x += 2) {
            for (int k = 0; k < 2; k++) {
                int xi = x + k;
                int c = row[xi * 2];
                int s = up[xi * 2] + dn[xi * 2] +
                        row[((xi > 0 ? xi - 1 : xi)) * 2] +
                        row[((xi + 1 < WIDTH ? xi + 1 : xi)) * 2];
                row[xi * 2] = (uint8_t)((c * 4 + s) >> 3);
            }
        }
    }
}

/* ---------- sensor controls ---------- */

static int ctrl_max(int fd, int id)
{
    struct v4l2_queryctrl q;
    memset(&q, 0, sizeof(q));
    q.id = id;
    if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0) return -1;
    return q.maximum;
}

static int ctrl_set(int fd, int id, int val)
{
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    c.value = val;
    return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static int ctrl_get(int fd, int id)
{
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0) return -1;
    return c.value;
}

/* is anyone else holding the virtual camera open? */
static int consumers_present(const char *node)
{
    DIR *p = opendir("/proc");
    if (!p) return 1;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(p))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid == getpid()) continue;
        char dir[64];
        snprintf(dir, sizeof(dir), "/proc/%d/fd", pid);
        DIR *fd = opendir(dir);
        if (!fd) continue;
        struct dirent *f;
        while ((f = readdir(fd))) {
            if (f->d_name[0] == '.') continue;
            char fp[300], target[512];
            snprintf(fp, sizeof(fp), "/proc/%d/fd/%s", pid, f->d_name);
            ssize_t n = readlink(fp, target, sizeof(target) - 1);
            if (n <= 0) continue;
            target[n] = 0;
            if (!strcmp(target, node)) { found = 1; break; }
        }
        closedir(fd);
    }
    closedir(p);
    return found;
}

/* ---------- main ---------- */


int main(int argc, char **argv)
{
    const char *capnode = NULL, *outnode = "/dev/video42", *subdev = "/dev/v4l-subdev6";
    int ae = 1, awb = 1, fixed_exp = -1, fixed_gain = -1;

    setvbuf(stdout, NULL, _IOLBF, 0);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) capnode = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outnode = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) subdev = argv[++i];
        else if (!strcmp(argv[i], "--no-ae")) ae = 0;
        else if (!strcmp(argv[i], "--no-awb")) awb = 0;
        else if (!strcmp(argv[i], "--exposure") && i + 1 < argc) fixed_exp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) fixed_gain = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            printf("usage: %s [-d capnode] [-o loopback] [-s subdev] [--no-ae] [--no-awb] [--exposure N] [--gain N]\n", argv[0]);
            return 0;
        }
    }

    char node[64] = {0};
    int port = -1;
    if (!capnode) {
        if (discover(node, sizeof(node), &port) < 0) {
            fprintf(stderr, "could not find gc2607 in the media graph\n");
            return 1;
        }
        capnode = node;
    } else {
        if (discover(node, sizeof(node), &port) < 0) port = 0;
    }
    printf("capture %s, csi2 port %d\n", capnode, port);
    setup_pipeline(port, capnode);

    struct capbuf bufs[NBUF] = {0};
    int cfd = -1, active = 0;
    double idle_since = 0, last_check = 0;
    int ofd = out_open(outnode);
    if (ofd < 0) return 1;
    /* v4l2loopback only advertises capture caps once the producer has sent a
       frame, so register with one black frame and keep the sensor off */
    {
        size_t bytes = (size_t)WIDTH * HEIGHT * 2;
        uint8_t *blank = malloc(bytes);
        if (blank) {
            for (size_t i = 0; i < bytes; i += 2) { blank[i] = 16; blank[i + 1] = 128; }
            if (write(ofd, blank, bytes) < 0) perror("blank frame");
            free(blank);
        }
    }
    char subdevbuf[300] = {0};
    if (!subdev) {
        if (find_subdev(subdevbuf, sizeof(subdevbuf)) == 0)
            subdev = subdevbuf;
        else
            subdev = "/dev/v4l-subdev6";
    }
    int sfd = open(subdev, O_RDWR);
    if (sfd < 0) perror("subdev");
    else printf("subdev %s\n", subdev);

    int maxexp = sfd >= 0 ? ctrl_max(sfd, V4L2_CID_EXPOSURE) : 1108;
    int gain_max = sfd >= 0 ? ctrl_max(sfd, V4L2_CID_ANALOGUE_GAIN) : 1012;
    int exp = fixed_exp > 0 ? fixed_exp : (sfd >= 0 ? ctrl_get(sfd, V4L2_CID_EXPOSURE) : 800);
    int gain = fixed_gain > 0 ? fixed_gain : (sfd >= 0 ? ctrl_get(sfd, V4L2_CID_ANALOGUE_GAIN) : 200);
    if (exp > maxexp) exp = maxexp;
    if (fixed_exp > 0 && sfd >= 0) ctrl_set(sfd, V4L2_CID_EXPOSURE, exp);
    if (fixed_gain > 0 && sfd >= 0) ctrl_set(sfd, V4L2_CID_ANALOGUE_GAIN, gain);
    printf("exposure range 1..%d, gain 64..%d, start exp=%d gain=%d\n", maxexp, gain_max, exp, gain);

    struct params P;
    params_init(&P);

    uint8_t *yuyv = malloc((size_t)WIDTH * HEIGHT * 2);
    if (!yuyv) return 1;

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    long frames = 0, printed = 0;
    double t0 = now_sec(), tl = t0;
    float luma = 0, mr = 0, mg = 0, mb = 0;

    while (running) {
        double tnow = now_sec();

        if (tnow - last_check > 0.5) {
            last_check = tnow;
            int someone = consumers_present(outnode);
            if (!someone) {
                if (!active) { usleep(200000); continue; }
                if (!idle_since) { idle_since = tnow; printf("idle: waiting for a consumer\n"); }
                else if (tnow - idle_since > 10.0) {
                    enum v4l2_buf_type ot = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    xioctl(cfd, VIDIOC_STREAMOFF, &ot);
                    struct v4l2_requestbuffers rb;
                    memset(&rb, 0, sizeof(rb));
                    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    rb.memory = V4L2_MEMORY_MMAP;
                    rb.count = 0;
                    xioctl(cfd, VIDIOC_REQBUFS, &rb);
                    for (unsigned i = 0; i < NBUF; i++)
                        if (bufs[i].start && bufs[i].start != MAP_FAILED) {
                            munmap(bufs[i].start, bufs[i].len);
                            bufs[i].start = NULL;
                        }
                    close(cfd);
                    cfd = -1;
                    active = 0;
                    idle_since = 0;
                    printf("idle: sensor stopped\n");
                }
            } else {
                idle_since = 0;
            }
        }

        if (!active) {
            if (!consumers_present(outnode)) { usleep(200000); continue; }
            setup_pipeline(port, capnode);
            cfd = cap_open(capnode, bufs);
            if (cfd < 0) { usleep(500000); continue; }
            active = 1;
            printf("consumer attached: streaming\n");
        }

        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        struct pollfd pfd = { .fd = cfd, .events = POLLIN };
        if (poll(&pfd, 1, 500) <= 0) continue;
        if (xioctl(cfd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            perror("DQBUF");
            break;
        }

        float stats[6] = {0};
        process_frame((const uint16_t *)bufs[b.index].start, yuyv, &P, stats);
        if (P.denoise) denoise_y(yuyv);

        if (write(ofd, yuyv, (size_t)WIDTH * HEIGHT * 2) < 0 && errno != EAGAIN)
            perror("loopback write");

        if (xioctl(cfd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); break; }

        frames++;
        if (stats[3] > 0 && stats[5] > 0) {
            mr = stats[0] / stats[3]; mg = stats[1] / stats[3]; mb = stats[2] / stats[3];
            luma = stats[4] / stats[5];
            if (awb && frames % 5 == 0) {
                float sr = (mr > 1e-3f) ? mg / mr : 1.0f;
                float sb = (mb > 1e-3f) ? mg / mb : 1.0f;
                if (sr < 0.85f) sr = 0.85f;
                if (sr > 1.18f) sr = 1.18f;
                if (sb < 0.85f) sb = 0.85f;
                if (sb > 1.18f) sb = 1.18f;
                P.awb[0] *= sr;
                P.awb[2] *= sb;
                if (P.awb[0] < 0.35f) P.awb[0] = 0.35f;
                if (P.awb[0] > 3.5f) P.awb[0] = 3.5f;
                if (P.awb[2] < 0.35f) P.awb[2] = 0.35f;
                if (P.awb[2] > 3.5f) P.awb[2] = 3.5f;
            }
        }

        if (ae && active && sfd >= 0 && frames % 5 == 0 && luma > 1.0f) {
            float err = 112.0f / luma;
            float factor = powf(err, 0.6f);
            if (factor < 0.5f) factor = 0.5f;
            if (factor > 1.8f) factor = 1.8f;
            if (factor > 1.02f) {
                if (exp < maxexp) {
                    int ne = (int)(exp * factor);
                    if (ne > maxexp) ne = maxexp;
                    if (ne != exp) { exp = ne; ctrl_set(sfd, V4L2_CID_EXPOSURE, exp); }
                } else if (gain < gain_max) {
                    int ng = (int)(gain * factor);
                    if (ng > gain_max) ng = gain_max;
                    if (ng != gain) { gain = ng; ctrl_set(sfd, V4L2_CID_ANALOGUE_GAIN, gain); }
                }
            } else if (factor < 0.98f) {
                if (gain > 64) {
                    int ng = (int)(gain * factor);
                    if (ng < 64) ng = 64;
                    if (ng != gain) { gain = ng; ctrl_set(sfd, V4L2_CID_ANALOGUE_GAIN, gain); }
                } else if (exp > 40) {
                    int ne = (int)(exp * factor);
                    if (ne < 40) ne = 40;
                    if (ne != exp) { exp = ne; ctrl_set(sfd, V4L2_CID_EXPOSURE, exp); }
                }
            }
        }

        double tn = now_sec();
        if (tn - tl >= 10.0) {
            printf("fps %.1f | luma %.0f | exp %d/%d | gain %d | awb %.2f %.2f %.2f | rgb %.4f %.4f %.4f\n",
                   (frames - printed) / (tn - tl), luma, exp, maxexp, gain,
                   P.awb[0], P.awb[1], P.awb[2], stats[3]>0?stats[0]/stats[3]:0, stats[3]>0?stats[1]/stats[3]:0, stats[3]>0?stats[2]/stats[3]:0);
            fflush(stdout);
            printed = frames;
            tl = tn;
        }
    }

    if (cfd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(cfd, VIDIOC_STREAMOFF, &t);
        close(cfd);
    }
    close(ofd);
    if (sfd >= 0) close(sfd);
    free(yuyv);
    return 0;
}
