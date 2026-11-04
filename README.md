# gc2607-own

视频会议摄像头的自研用户态管线。作者自评：**这是 vibe 的一坨**。

## 这是什么

HUAWEI MateBook GT 14（ENZH-XX）的前置摄像头是 GalaxyCore GC2607：1920x1080、
RAW10 Bayer GRBG、MIPI CSI-2 双 lane，挂在 Intel IPU6 上，供电和时钟由 INT3472 提供。
Linux 内核没有这颗传感器的驱动，Intel 的 HAL 也不认它，所以这里从零搭了一条能用的链路：

```
gc2607.ko (V4L2 sensor)
  -> Intel IPU6 ISYS (内核自带)  /dev/videoN, SGRBG10
  -> gc2607d (本项目)            去马赛克 / AWB / CCM / gamma / 降噪 -> YUYV
  -> v4l2loopback                /dev/video60 "GC2607 Own"
```

`gc2607d` 是单文件 C 程序，不依赖 OpenCV、libcamera 或任何 Intel 闭源库。

## 目录

```
src/gc2607d.c                     采集 + 图像处理 + AE/AWB + 输出守护进程
kernel/gc2607.c                   传感器驱动（V4L2 subdev，CCI 寄存器接口）
kernel/Makefile, dkms.conf        外部模块构建 / DKMS
kernel/0001-ipu-bridge-add-gc2607.patch
                                  给内核 ipu-bridge 加 GCTI2607 的传感器表项
scripts/build.sh                  编译守护进程和两个内核模块
scripts/run.sh                    加载 v4l2loopback 并前台运行管线
scripts/install-boot.sh           装成开机自启（写 /lib/modules、modules-load.d）
```

## 依赖

- 内核头文件（与运行内核版本一致）
- media-ctl、v4l-utils
- gcc、libgomp（`-fopenmp`）
- v4l2loopback（从源码编译，`scripts/build.sh` 会提示）

## 构建

```sh
./scripts/build.sh
```

产物体积很小，构建过程不下载任何二进制包。

## 运行

```sh
sudo ./scripts/run.sh
```

然后在应用里选 **GC2607 Own**。

## 当前状态

- 1920x1080@30.0fps，AE 稳定在 luma 113 左右，AWB 逐帧收敛
- 室内光线下增益拉得高，噪点明显；降噪目前只有 3x3 平均
- 未做：时域降噪、锐化、HDR、60fps（需要重调 PLL 和 MIPI 速率）
- 传感器固定挂在 CSI2-4，`gc2607d` 会自己从 media graph 里找端口和节点

## 致谢

驱动骨架和 30fps 的时序修法来自
[abbood/gc2607-v4l2-driver](https://github.com/abbood/gc2607-v4l2-driver) 和
[AlexDaichendt/gc2607-camera-linux](https://github.com/AlexDaichendt/gc2607-camera-linux)。
上游用的是 Intel HAL/PSYS 那一套（闭源二进制），在本机枚举不到这颗传感器，所以这里换成了纯自研的用户态处理。

## 许可

`src/` 下为 MIT。`kernel/gc2607.c` 为 GPL-2.0。
