#!/usr/bin/env bash
# One-shot installer: fetch everything from GitHub, build from source, install.
#   curl -fsSL https://raw.githubusercontent.com/G-549/gc2607-own/main/scripts/install-online.sh | sudo bash
# Env: REF (default v0.2.0), DRY_RUN=1, NO_LOOPBACK=1
set -euo pipefail

REPO="G-549/gc2607-own"
REF="${REF:-v0.2.0}"
KV="$(uname -r)"
KVER="${KV%%-*}"
KDIR="${KDIR:-/lib/modules/$KV/build}"
UPDATES="/lib/modules/$KV/updates"
WORK="$(mktemp -d)"
DRY="${DRY_RUN:-0}"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
run() { if [[ "$DRY" == 1 ]]; then printf '+ %s\n' "$*"; else "$@"; fi; }

[[ $EUID -eq 0 ]] || die "run as root"
[[ -d "$KDIR" ]] || die "kernel headers missing: $KDIR (install linux-lts-headers)"
command -v gcc >/dev/null || die "gcc missing"
command -v curl >/dev/null || die "curl missing"
trap 'rm -rf "$WORK"' EXIT

say "source: $REPO @ $REF"
URL="https://github.com/$REPO/archive/refs/tags/$REF.tar.gz"
[[ "$REF" == "main" ]] && URL="https://github.com/$REPO/archive/refs/heads/main.tar.gz"
curl -fsSL --connect-timeout 15 --max-time 300 --retry 2 "$URL" -o "$WORK/src.tar.gz"
tar xzf "$WORK/src.tar.gz" -C "$WORK"
SRC="$(find "$WORK" -maxdepth 1 -type d -name 'gc2607-own-*' | head -1)"
[[ -n "$SRC" ]] || die "unexpected archive layout"

say "build daemon"
run make -C "$SRC/src"

say "build gc2607 sensor module"
run make -C "$SRC/kernel" KDIR="$KDIR"

if [[ "${NO_LOOPBACK:-0}" != 1 ]]; then
    say "build v4l2loopback"
    curl -fsSL --connect-timeout 15 --max-time 300 --retry 2 "https://github.com/v4l2loopback/v4l2loopback/archive/refs/heads/main.tar.gz" -o "$WORK/lb.tar.gz"
    tar xzf "$WORK/lb.tar.gz" -C "$WORK"
    run make -C "$WORK/v4l2loopback-main" KERNEL_DIR="$KDIR"
fi

say "build patched ipu-bridge (sensor table entry)"
curl -fsSL --connect-timeout 15 --max-time 300 --retry 2 "https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/plain/drivers/media/pci/intel/ipu-bridge.c?h=v$KVER" -o "$WORK/ipu-bridge.c"
grep -q 'GCTI2607' "$WORK/ipu-bridge.c" || \
    sed -i 's/\tIPU_SENSOR_CONFIG("OVTI01AS", 1, 400000000),/&\n\t\/* GalaxyCore GC2607 *\/\n\tIPU_SENSOR_CONFIG("GCTI2607", 1, 336000000),/' "$WORK/ipu-bridge.c"
grep -q 'GCTI2607' "$WORK/ipu-bridge.c" || die "patch did not apply"
printf "obj-m += ipu-bridge.o\nall:\n\t\$(MAKE) -C \$(KDIR) M=\$(CURDIR) modules\n" > "$WORK/Makefile"
run make -C "$WORK" KDIR="$KDIR"

say "install"
run install -d "$UPDATES"
run install -m644 "$SRC/kernel/gc2607.ko" "$UPDATES/"
[[ -f "$WORK/ipu-bridge.ko" ]] && {
    SYS="/lib/modules/$KV/kernel/drivers/media/pci/intel/ipu-bridge.ko.zst"
    [[ -f "$SYS" && ! -f "$SYS.orig" ]] && run cp -a "$SYS" "$SYS.orig"
    if [[ "$DRY" != 1 ]]; then
        zstd -q -f -19 "$WORK/ipu-bridge.ko" -o "$SYS"
    fi
}
[[ -f "$WORK/v4l2loopback-main/v4l2loopback.ko" ]] && run install -m644 "$WORK/v4l2loopback-main/v4l2loopback.ko" "$UPDATES/"
run depmod -a

run install -m755 "$SRC/src/gc2607d" /usr/bin/gc2607d
run install -m644 "$SRC/systemd/gc2607-camera.service" /usr/lib/systemd/system/
run install -Dm644 "$SRC/config/wireplumber/51-hide-ipu6.conf" /etc/wireplumber/wireplumber.conf.d/51-hide-ipu6.conf
printf 'options v4l2loopback video_nr=60 card_label="GC2607 Own" exclusive_caps=1\n' > "$WORK/modprobe.conf"
run install -Dm644 "$WORK/modprobe.conf" /etc/modprobe.d/gc2607-v4l2loopback.conf
echo v4l2loopback > "$WORK/modules-load.conf"
run install -Dm644 "$WORK/modules-load.conf" /etc/modules-load.d/v4l2loopback.conf
echo gc2607 > "$WORK/gc2607-load.conf"
run install -Dm644 "$WORK/gc2607-load.conf" /etc/modules-load.d/gc2607.conf

say "enable service"
if [[ "$DRY" == 1 ]]; then
    echo "+ systemctl daemon-reload && systemctl enable --now gc2607-camera.service"
else
    systemctl daemon-reload
    systemctl enable --now gc2607-camera.service
fi

say "done"
[[ "$DRY" == 1 ]] && echo "(dry run: nothing installed)"
systemctl --no-pager --lines=3 status gc2607-camera.service 2>/dev/null | head -5 || true
