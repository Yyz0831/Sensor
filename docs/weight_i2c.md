# TAL220B + M5Stack Unit Weight-I2C（U180）

独立 PlatformIO 环境为 **`weight_i2c`**，也是当前默认环境。
旧温度环境 `nucleo_f411re` 和一体秤环境 `mini_scales` 继续保留。
此测试读取 U180 的原始 ADC，启动时加载固件中的默认重量系数，由 Nucleo 在 RAM 中计算零点。
不使用模块已有的重量系数、不读取 Mini Scales 按钮，也不限制为 5 kg。

## 接线与安装

断电后，将 TAL220B 接到 U180 的称重端子，按**端子丝印**而非连接器左右方向核对：

| TAL220B 线色 | U180 端子 |
| --- | --- |
| 红 | E+（激励正） |
| 黑 | E−（激励负） |
| 绿 | S+（信号正） |
| 白 | S−（信号负） |

U180 Grove 连接 Nucleo-F411RE：

| Grove 线色 | Nucleo |
| --- | --- |
| 红 | 5V（Nucleo 使用 ST-Link USB 供电） |
| 黑 | GND |
| 黄 | PB9 / D14（SDA） |
| 白 | PB8 / D15（SCL） |

注意两组线用途不同：TAL220B 的白线是 S−，Grove 的白线是 SCL。
传感器由 U180 的 E+/E− 激励，不要另把传感器信号接到 Nucleo 的 A0 或 GPIO。
U180 官方原理图 R9/R10 将 SDA/SCL 上拉到内部 3.3 V，保持现有 I²C1 100 kHz 配置即可。
**先断开 Mini Scales**：两者默认地址均为 `0x26`，不能以相同地址同时接在本总线上。

TAL220B 是梁式传感器。按本体箭头、安装图识别固定端和受力端；固定端锁在刚性底座，
另一端安装承重平台。用垫块留出活动空间，使中部形变区域及受力端不会碰到底座。
不要把整根传感器平压在桌面，或将两端都固定到同一底座；线缆也不能拉扯受力端。
通用手册列有 2/3/5/10/20/50 kg，多型号共用一份手册，不能据此确定实物量程。
先看本体标签确认量程，再做较大载荷测试；程序没有机械过载保护。

## 编译、上传和串口

在 `Sensoring` 目录运行（上传由你操作）：

```sh
~/.platformio/penv/bin/pio run -e weight_i2c
~/.platformio/penv/bin/pio run -e weight_i2c --target upload
~/.platformio/penv/bin/pio device monitor -e weight_i2c --baud 115200
```

VS Code 可选 **PlatformIO → Project Tasks → weight_i2c → Upload / Monitor**。
`pio run` 默认编译此环境；显式选旧环境任务仍会上传旧程序。
复位后确认看到 `TAL220B + Unit Weight-I2C U180`。每 250 ms 输出一次，LD2 每 500 ms 翻转。
波特率 115200、8N1、UTF-8。关闭其他占用相同串口的程序。
本环境启用了 `send_on_enter` 和本地回显：输入整行英文命令后按 Enter。
串口工具也可发送 LF、CR 或 CRLF。命令区分大小写。

```text
时间=1000 ms | 原始ADC=123456 | 重量=待去皮 | 状态=正常
时间=8000 ms | 原始ADC=145678 | 重量=500.00 g | 状态=正常
时间=8250 ms | 原始ADC=不可用 | 重量=nan g | 状态=I2C通信失败
```

这些数值仅作格式示例。正常通信不代表载荷传感器接线或机械安装正确；
未接传感器时 ADC 也可能返回数值。上电后未完成去皮前，不将 ADC 直接显示为克数；
没有有效系数时显示“未校准”。
断线会在下一次采样自动重试，未将读取失败伪装为零重量。

## 去皮和校准

### 日常使用：复用已记录系数

用户已确认使用**实际 100 g 砝码**得到以下系数，已记录在 `Core/Inc/weight_profile.h`：

```c
#define WEIGHT_PROFILE_COUNTS_PER_GRAM 1082.79688
#define WEIGHT_PROFILE_REFERENCE_G    100.0
```

重新编译、上传此版本后，复位或断电再启动会自动加载该默认系数。
每次上电只需空载稳定后发送 `tare`，等待约 4 秒，即可直接称重；无需再放砝码执行 `cal`。
零点不固化，避免把上一次的载荷或装配状态误用为当前零点，也不会带载自动清零。

默认系数只适用于当前传感器和安装方式；更换传感器、接线极性或机械安装后应重新校准。
可放上100 g砝码复核，应显示约100 g；取下应回到约0 g。

### 需要重新校准时

1. 安装完成，空载放平，等原始 ADC 稳定。发送 `tare` 后保持空载约 4 秒，等“去皮完成”。
2. 放上**已知质量**的物品，等原始 ADC 稳定。
3. 发送 `cal 500`，其中 `500` 必须换成实际质量，单位 g。保持载荷约 4 秒，等“校准完成”。
4. 检查当前显示是否接近参考质量，移除物品检查回零，再用另一个已知质量检查比例。

命令：

```text
tare
cal 500
help
```

### 串口一直滚动时，用独立工具发送命令

滚动时仍可点击串口终端，使用英文输入法键入命令后按 Enter；输出可能把输入显示打散。
也可先用 Ctrl+C 退出串口监视器，保持板子连接，在 `Sensoring` 目录用普通终端执行：

```sh
# 空载稳定后执行，等待“去皮完成”
.venv/bin/python tools/weight_control.py --port /dev/cu.usbmodem1103 tare

# 可选：需要重新校准时，放上50 g砝码并稳定后执行（质量必须与命令一致）
.venv/bin/python tools/weight_control.py --port /dev/cu.usbmodem1103 cal 50

# 再打开实时数值
~/.platformio/penv/bin/pio device monitor -e weight_i2c --baud 115200
```

端口换成 `pio device list` 显示的实际 ST-Link 串口。两次命令之间不要复位或拔插板子，
以免 RAM 中的零点丢失。工具只显示命令提示和结果，不刷实时数据；无需重新编译或烧录。
工具会先核对板上 `weight_i2c` 帮助回复，不会替你自动去皮或自动放置砝码。

每次去皮或校准收集 16 次有效 ADC，采样间隔 250 ms，平均后一次性更新。
`零点 = 空载ADC平均值`；`系数 = (加载ADC平均值 − 零点) / 参考质量g`；
`重量g = (当前ADC − 零点) / 系数`。负系数允许传感器极性相反，负重量不会被取绝对值。
已启用有效固件默认系数时，去皮后直接显示重量；默认系数禁用或无效时才需 `cal`。
已校准后再次 `tare` 只更新零点，保留当前系数。
采集中再次输入 `tare`/`cal` 会提示忙，`help` 仍可用。

质量必须为有限正数，不能附加 `g` 字符；必须先去皮再校准。
ADC 零变化、无效计算或中途 I²C 失败都会拒绝本次更新，保留原有零点和校准。
每次采集开始前需自行等待稳定，程序不自动判断物体是否停止晃动。
命令过长或串口错误会丢弃整条命令；按 Enter 清除后重新输入。

**默认系数固化在程序中，重启后恢复；零点和手动 `cal` 更新的系数只在 RAM 中。**
后续手动 `cal` 不会自动改变固件默认值；若要长期使用新系数，将其填入
`Core/Inc/weight_profile.h` 的 `WEIGHT_PROFILE_COUNTS_PER_GRAM`，并同步更新参考质量，重新编译上传。
将默认系数设为 `0.0` 可恢复每次手动去皮、校准的方式。
程序不向模块写入零点、GAP、地址、滤波或固件，也不在运行时写 Nucleo Flash。

## 验证与故障定位

```sh
.venv/bin/python -m unittest discover -s test -p 'test_*.py' -v
~/.platformio/penv/bin/pio run -e weight_i2c -e mini_scales -e nucleo_f411re
```

主机测试模拟 I²C，检查真实驱动的原始值及符号、断线恢复、16 次平均、
正负系数、失败不覆盖校准、重复去皮、参考质量及命令解析。
这些测试不能替代板上的 UART 中断接收、真实采样时序、传感器和机械安装验证。

若只有温度CSV或 Mini Scales 提示，重新上传 **weight_i2c** 环境并复位检查启动文字。
若 I²C 失败，检查 5V/GND/SDA/SCL、是否冲突地址以及模块地址是否曾被更改。
若原始 ADC 一直不变或接近极限，先检查 E+/E−/S+/S− 和安装方式，不能仅靠去皮解决。
更换传感器或改变安装方式后重新校准。复位后需要重新去皮，手动校准系数会恢复为固件默认值。

## 资料

- [TAL220B 手册](https://cdn.sparkfun.com/assets/e/5/f/5/6/TAL220B.pdf)
- [Unit Weight-I2C 文档](https://docs.m5stack.com/en/unit/Unit_Weight-I2C)
- [U180 原理图](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/632/SCH_UNIT_WEIGHT_I2C.pdf)
- [官方协议](https://github.com/m5stack/M5Unit-WeightI2C/blob/main/docs/Unit_Weight_I2C_Protocol.pdf)
- [官方库寄存器定义](https://github.com/m5stack/M5Unit-WeightI2C/blob/main/src/M5UnitWeightI2C.h)

HAL 设备地址为 `0x26 << 1`，原始 ADC 在 `0x00`，四字节小端带符号数据。
版本读 `0xFE`，与官方库一致；协议 PDF 末尾的 `0xF0` 标注存在不一致。
