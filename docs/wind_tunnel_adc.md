# Wind Tunnel Logger：原始 ADC 标定模式

PlatformIO 环境 **`wind_tunnel_adc`** 是当前默认环境，适配课程官方
`wind-tunnel-logger 1.0.0`。硬件仍为 Nucleo-F411RE + TAL220B + Unit Weight-I2C，
接线见 [称重说明](weight_i2c.md)。当前先采集 ADC 与真实风速，后续再建立风速换算。

已有 2026-10-29 三组标定数据的同一安装可使用新增的
[风速模式 `wind_tunnel_speed`](wind_tunnel_speed.md)：选择该环境上传，关风后按 RESET 自动校零约 4 秒，
再向 Logger 输出 m/s。默认环境仍保留为本页的原始 ADC 模式。

## 构建、烧录与连接

在 `Sensoring` 目录执行；上传由用户操作：

```sh
~/.platformio/penv/bin/pio run -e wind_tunnel_adc
~/.platformio/penv/bin/pio run -e wind_tunnel_adc --target upload
```

VS Code 中也可选择 **PlatformIO → Project Tasks → wind_tunnel_adc → Upload**。
普通 `pio run` / 默认 Upload 使用这个环境。烧录后关闭 PlatformIO Monitor、
`weight_logger.py` 和其他占用同一串口的软件，再启动课程 Logger：

```sh
/Users/yyz/python/Sensoring/Lab1/WindTunnelLogger/start.command
```

在 **Your board** 选择 Nucleo 的 ST-Link 串口（例如 `/dev/cu.usbmodem1103`），
使用 **115200 baud** 连接。正式采集选择实际风洞作为 Ground truth。
`Simulated (bench test)` 只验证连接和保存，生成的参考风速不能用于标定。
每种板子、安装方式和测试轮次使用可辨认的保存名称，例如 `plate_100x100_run1_adc`。

## 串口协议与时序

串口为 **115200、8N1**。有效数据期间目标 **每5 ms发送一行，即约200行/秒**，
每行只有不带缩放的原始ADC十进制整数和LF换行。例如：

```text
8769386
8769386
8769386
8769533
```

上例仅说明格式。支持负整数和真实零值；不减去零点、不使用克数系数、不截断、不做风速换算。
没有表头、前缀、单位、时间戳、启动提示或调试文字。
本模式忽略串口输入，`tare`、`cal`、`help` 不会产生回复。

I²C每100 ms读取一次模块，独立的SysTick调度每5 ms用UART中断发送最近的有效读数。
阻塞的I²C事务在主循环中执行，正常发送不依赖主循环读取是否正在等待。
最大整数行12字节，200行/秒在8N1下占24,000 bit/s，小于115200 baud。
错过的发送时隙或忙碌的UART会跳过，不会事后突发补发。
实际速率和抖动仍需用物理板子在Logger中确认；HSI时钟和USB接收分包会带来偏差。

**200 Hz是串口输出率，模块独立转换仍约10 Hz。**相邻多行可以来自同一读数，
不能把这些保持值当成200次独立测量。模块没有提供转换序号，ADC数值不变也不能证明没有新转换。
之前 `weight_logger.py --output-hz 200` 是停止后重采样CSV；课程Logger使用这里的直接串口模式，
不需要运行该脚本。

## 缺失与故障行为

- 启动先静默约500 ms，并等到首次成功的I²C读取后才发送数值。
- 发现I²C读取失败后立即使缓存失效，停止发送；不会把驱动返回的占位零输出为测量。
- 最近成功读取超过250 ms未更新时停止发送，防止持续重放过期读数。
- 每100 ms继续尝试读取，成功后恢复5 ms发送节奏。
- 错误时不发送 `nan`、哨兵整数或错误文字。Logger会看到缺失/速率下降，持续无数据后显示NO DATA。

通信失败需要到诊断环境排查。I²C成功本身不能确认TAL220B的机械安装或信号线接法正确，
仍需用加载、卸载和断线试验验证。

## 当前CSV列的实际单位

官方Logger源码保持原样，其界面仍标为m/s，默认显示范围约为-2到100，
所以大ADC可能显示为顶端的100。这不表示固件ADC被截断。

| 文件/列 | 当前实际含义 | 标定用途 |
| --- | --- | --- |
| `_student.csv` → `value_raw_mps` | **ADC counts**，未裁剪的串口原值 | 使用 |
| `_student.csv` → `value_clipped_mps` | 为界面显示裁剪后的值 | 不使用 |
| `_aligned.csv` → `student_mps` | **ADC counts**，与参考数据配对的未裁剪原值 | 使用 |
| `_aligned.csv` → `ground_truth_mps` | 风洞参考风速，单位 **m/s** | 使用 |

每次改变迎风板或安装方式后记录无风基线，再逐档稳定采集，并保存板子尺寸、安装角度和轮次。
后续用原始ADC或相对该轮无风基线的ADC变化量与真实风速拟合，在独立轮次上验证。
正式标定只使用真实风洞参考数据，保留官方导出文件及其原始列名，另行注明本轮Student单位为ADC。
保持行不增加独立样本数；按稳定测试段汇总或按原始有效更新率处理。
现在不能把ADC与m/s相减来计算速度误差，也不能用Logger内置的速度误差/评分评价当前板子。
拟合完成并验证后，才把这一列的数值换成实际风速m/s，继续沿用单数字行协议。

## 返回诊断/校准模式

```sh
~/.platformio/penv/bin/pio run -e weight_i2c --target upload
~/.platformio/penv/bin/pio device monitor -e weight_i2c --baud 115200
```

`weight_i2c` 保留中文标签、约10 Hz输出及 `tare` / `cal` / `help`；
原温度环境为 `nucleo_f411re`，Mini Scales为 `mini_scales`。
切换固件会重启板子，RAM零点需按原流程重新去皮。所有已有CSV和固件默认克数系数保留。
再连接课程Logger前，应重新上传 `wind_tunnel_adc`，并关闭诊断串口监视器。

## 验证范围

```sh
.venv/bin/python -m unittest discover -s test -p 'test_*.py' -v
~/.platformio/penv/bin/pio run -e wind_tunnel_adc -e weight_i2c -e mini_scales -e nucleo_f411re
```

离线测试编译实际C格式化/调度代码，验证整数边界、5 ms节奏、10秒2000行、启动/故障/过期静默、
恢复、时钟回绕及UART忙时不补发。若旁边已安装课程Logger，还将C代码生成的200行输入
官方解析器，核对无非法行，以及原始/配对ADC未被裁剪。
这些检查不等于物理UART、ST-Link USB、真实模块或风洞验证。
烧录后还需确认正常接收约200 Hz、实际加载ADC变化、断开模块后停止更新、恢复后继续接收，
并核对实际保存文件中的原始ADC与真实风速列。
