# 踩过的坑

- **传感器不在 CSI2-0**：这台机器上 `ipu_bridge` 把它挂到 CSI2-4，
  所以别信「默认端口 0」，每次从 media graph 里自己找。
- **别 unbind 传感器驱动**：`echo ... > /sys/bus/i2c/drivers/gc2607/unbind` 之后
  INT3472 会认为没有依赖方，把电源关掉，重新加载读 ID 直接 -121，只能重启。
- **卸载顺序**：先 `modprobe -r intel-ipu6-isys` 再 `rmmod gc2607`；
  直接卸 `intel_ipu6` 会踩到 `ipu6_buttress_isr`，模块引用计数变 -1。
- **i2c 地址被驱动占着**：驱动加载后 `/dev/i2c-3` 无法再对 0x37 发包，
  想运行时改寄存器只能走驱动自己的参数。
- **/dev/video42 不是空闲的**：IPU6 自己占了 0–47，
  v4l2loopback 要用 60 这种编号。
- **Intel HAL 那条路**：psys 起来了、AIQB 也装了，但 HAL 的 graph xml 里要
  `Intel IPU6 CSI2 BE SOC 0` 这种实体，主线 isys 不生成，
  所以枚举不到相机，最终放弃。
