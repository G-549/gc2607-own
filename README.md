# gc2607-own

> ## ⚠️ 这是 vibe 的一坨
> 记录于 2026-11-04，由一个不会用 Intel HAL 的 agent 边试边写。
> 能跑，但别当参考实现。

MateBook GT 14（ENZH-XX）那颗在 Linux 上没人管的摄像头，现在能用了：
1080p、30fps、自动曝光和白平衡，在浏览器和会议软件里当成普通摄像头出现。

```
GC2607 传感器  →  gc2607.ko  →  IPU6 ISYS  →  gc2607d  →  v4l2loopback
   1920x1080        V4L2          raw 节点      去马赛克       "GC2607 Own"
   RAW10 GRBG       sensor                       白平衡/CCM
   2 lane MIPI                                    /gamma/降噪
```

## 为什么有这东西

内核里没有这颗传感器的驱动，Intel 那套 HAL 又只认它自己表里的型号，
枚举不到这台机器上的 GC2607。所以最小可用的一条路是自己写：一个 V4L2 驱动，
一个用户态处理程序，中间不经过任何闭源库。

## 现在能做到什么

- 1920×1080 @ 30.0fps，帧间隔稳定
- 自动曝光（目标亮度 112/255）和逐帧收敛的灰度世界白平衡
- 双线性去马赛克、3×3 CCM、sRGB gamma、轻度降噪
- 输出为标准 v4l2loopback 设备，OBS、Chrome、会议软件都能选
- 按需取流：没人用虚拟摄像头时不碰传感器，指示灯不亮

## 现在还不行

- 室内光线下增益拉满，噪点明显；没有时域降噪，没有锐化
- 60fps 需要重调 PLL 和 MIPI 速率，没做
- 想拿它当主力直播摄像头还差点意思，当视频会议够用

## 跑起来

一行装完（脚本从 GitHub 取源码，本机编译，不下载任何二进制包）：

```sh
curl -fsSL https://raw.githubusercontent.com/G-549/gc2607-own/main/scripts/install-online.sh | sudo bash
```

或者自己来：

```sh
./scripts/build.sh              # 编译守护进程和内核模块
sudo ./scripts/install-boot.sh  # 装模块和 systemd 服务
```

之后在应用里选 **GC2607 Own**。服务名 `gc2607-camera`，日志用
`journalctl -u gc2607-camera` 看。

需要内核头文件、gcc、v4l-utils、media-ctl、v4l2loopback。

## 细节

- 构建环境与可复现：`docs/BUILD.md`
- 处理链和各参数的来由：`docs/DESIGN.md`
- 踩过的坑：`docs/PITFALLS.md`
- 屏蔽 IPU6 那 48 个空节点：`config/wireplumber/51-hide-ipu6.conf`

## 致谢

驱动骨架和 30fps 的时序修法来自
[abbood/gc2607-v4l2-driver](https://github.com/abbood/gc2607-v4l2-driver) 与
[AlexDaichendt/gc2607-camera-linux](https://github.com/AlexDaichendt/gc2607-camera-linux)。
上游走的是 Intel HAL/PSYS（闭源二进制），在本机枚举不到传感器，这里换成了纯自研的用户态处理。

`src/` MIT，`kernel/gc2607.c` GPL-2.0。
