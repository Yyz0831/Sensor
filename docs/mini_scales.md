# M5Stack Unit Mini Scales 独立称重测试

适用于 U177（5 kg），I²C 默认地址 `0x26`。模块包含 HX711 和控制 MCU，
Nucleo 直接读取重量寄存器，不需要连接或控制模块内部的 HX711 时钟线。

## 接线

断电后连接模块的 Grove 四根线，核对连接器上的标识：

| Grove 线色 | 模块信号 | Nucleo-F411RE |
| --- | --- | --- |
| 黑 | GND | GND |
| 红 | 5V | 5V（板子由 ST-Link USB 供电） |
| 黄 | SDA | PB9 / D14 |
| 白 | SCL | PB8 / D15 |

模块电源接 **5V**。官方 V1.0 原理图中，SDA/SCL 由 R9/R10 各 4.7 kΩ 上拉到模块内部的
**3.3V**，所以可直接连接 Nucleo 的 I²C 引脚，通常不需要额外上拉。
不要把 SDA/SCL 额外上拉到 5V。GPIO 配置继续使用现有的 I²C1、100 kHz、开漏模式。
本环境不采集温度，TMP 和 MCP9808 可以移除；无需连接 A0。

## 构建、上传、查看数据

在 `Sensoring` 目录运行：

```sh
pio run -e mini_scales
pio run -e mini_scales --target upload
pio device monitor -e mini_scales --baud 115200
```

如果 `pio` 不在 PATH，可使用 `/Users/yyz/.platformio/penv/bin/pio`。
VS Code 的 PlatformIO Project Tasks 中，选择 **mini_scales → General → Upload / Monitor**。
仅编译不会切换板上程序，需要上传这个环境。串口端口可通过 `pio device list` 查看。

程序每 250 ms 输出一行带中文标签的文本（UTF-8，4 Hz 轮询，不代表模块内部转换率）：

```text
时间=1000 ms | 重量=125.30 g | 原始ADC=123456 | 按钮=松开 | 状态=正常(0)
时间=1250 ms | 重量=nan g | 原始ADC=不可用 | 按钮=不可用 | 状态=I2C通信失败(1)
```

上面的数值只是格式示例。时间为板子启动后的毫秒数，重量单位为克，原始 ADC 是带符号的原始值；
按钮直接显示“按下”或“松开”。状态同时显示中文说明和代码：`0` 正常、`1` I²C 失败、`2` 返回数据无效、
`3` 重量超出 ±5000 g。失败重量显示 `nan`，I²C 失败后会在下次采样重试。
以 `#` 开头的行为启动或去皮提示。LD2 每 500 ms 翻转，表示主循环在运行。

## 去皮与检查

1. 安装好称重板并放平，保持模块底部固定，避免导线或物体顶住称重部分。
2. 空载等待读数稳定，然后按住**Mini Scales 模块自身的按钮**至少 0.3 秒再松开。
   程序在按下沿发送一次去皮指令，不会在启动时自动清零。
3. 等待读数重新稳定，放上已知质量的物品，检查克数以及移除后的回零情况。

去皮只调整零点，不是重量增益校准。程序不修改 GAP 校准系数、I²C 地址或滤波配置。
若比例明显不对，需要按官方已知砝码流程检查校准；不要直接给重量乘一个未知系数。
固件 v3 可能限制负重量，v4 移除了该限制；本测试会保留模块返回的有效负数，不主动升级模块固件。
软件范围检查不能防止机械过载，不要超过额定 5 kg。

## 保留温度实验

原来的 `main.c`、TMP 临时 +50 °C 和 Python 温度程序保留。
当前默认环境为 TAL220B + Unit Weight-I2C 的 `weight_i2c`。
Mini Scales 请显式选择 `mini_scales`；切回温度固件时显式选择 `nucleo_f411re`：

```sh
pio run -e nucleo_f411re --target upload
```

`tools/temperature_logger.py` 只识别温度 CSV，当前称重输出是带标签文本，请使用串口监视器。
运行前关闭其他占用同一串口的程序。

## 协议与资料

- [产品文档及线序](https://docs.m5stack.com/en/unit/Unit_Mini_Scales)
- [V1.0 原理图](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/629/SCH_UNIT_MiniScales_V1.0.pdf)
- [官方库寄存器定义](https://github.com/m5stack/M5Unit-Miniscale/blob/main/src/UNIT_SCALES.h)
- [官方库读写实现](https://github.com/m5stack/M5Unit-Miniscale/blob/main/src/UNIT_SCALES.cpp)

重量寄存器 `0x10` 为小端 float32，单位 g；原始 ADC 为 `0x00`；按钮为 `0x20`；
向 `0x50` 写入一个字节 `1` 去皮。HAL 中地址使用 `0x26 << 1`。
固件版本使用官方库的 `0xFE`；协议 PDF 末两行存在把版本、地址都写作 `0xF0` 的不一致。

## 本机验证

```sh
.venv/bin/python -m unittest discover -s test -p 'test_*.py' -v
```

称重驱动测试模拟 I²C，检查实际 C 驱动的地址与寄存器、小端浮点重量、负重量、
带符号 ADC、通信失败和恢复、无效值以及去皮命令。实际称重和按键操作仍需上传后验证。
