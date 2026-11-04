# 构建与复现

## 验证过的环境

| 项 | 值 |
|---|---|
| 发行版 | Arch Linux |
| 内核 | linux-lts 6.18.60-1（`6.18.60-1-lts`） |
| 内核头文件 | 与运行内核完全一致 |
| gcc | 16.2.1 20260810 |
| CPU | Intel Core Ultra 7 155H（18 线程） |

编译器版本会影响优化结果，但**不影响功能**。目标是同一套源码在同一内核上编译出行为一致的模块。

## 可复现的做法

1. 编译选项固定，不用 `-march=native`（那会让产物跟机器绑定，
   且不同 CPU 编出来的二进制不同）。守护进程只用 `-O2`。
2. 不生成时间戳：`SOURCE_DATE_EPOCH` 默认沿用源码提交时间。
3. `LC_ALL=C`，避免本地化影响编译器输出和排序。
4. 内核模块必须用**运行内核的头文件**编译，版本号不一致会直接拒绝加载（vermagic）。

```sh
./scripts/build.sh          # 会打印本机工具链版本，便于对照
./scripts/versions.sh       # 只打印版本，不编译
```

## 内核模块

```sh
make -C kernel KDIR=/lib/modules/$(uname -r)/build
```

`ipu-bridge` 是内核自带的模块，需要先打补丁再加编译：

```sh
# 在对应版本的内核源码树里
git apply kernel/0001-ipu-bridge-add-gc2607.patch
# 或者直接改 drivers/media/pci/intel/ipu-bridge.c，加一行：
#   IPU_SENSOR_CONFIG("GCTI2607", 1, 336000000),
```

打补丁后的模块要覆盖 `/lib/modules/$(uname -r)/.../ipu-bridge.ko.zst` 并 `depmod`，
否则传感器不会出现在 media graph 里。

## 常见失败

- `No rule to make target scripts/Kbuild.include`：头文件包没装全，或者解出来的头文件树不完整。
- `Unknown symbol cci_read / devm_cci_regmap_init_i2c`：缺 `v4l2-cci`，`modprobe v4l2-cci`。
- `insmod: File exists`：`intel_ipu6_isys` 还占着异步 notifier，先 `modprobe -r intel-ipu6-isys`。
- 传感器读 ID 返回 `-121`：INT3472 把电源关了，reboot 最省事。
