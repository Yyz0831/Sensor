# Lab 1 双温度传感器采集

当前默认环境为 [TAL220B + Unit Weight-I2C 测试](docs/weight_i2c.md)：`weight_i2c`，
支持原始 ADC、串口 `tare` 和 `cal 质量g` 校准。
当前已记录实际100 g砝码测得的默认系数 `1082.79688 ADC/g`；上传当前固件后，
每次上电空载执行 `tare` 即可称重。手动 `cal` 的后续更新只在本次运行有效。
[Mini Scales 独立称重测试](docs/mini_scales.md) 保留为 `mini_scales`；
原温度实验保留为 `nucleo_f411re`，请显式选择对应环境上传。

Nucleo-F411RE 每 250 ms 读取 MCP9808 和 TMP36，通过 ST-Link 虚拟串口输出 CSV。
板载绿色 LD2 继续亮 500 ms、灭 500 ms。没有接上传感器也可以运行；失败读数输出 `nan`。

## 接线

| 传感器引脚 | Nucleo 接口 |
| --- | --- |
| TMP36 V+ | 3V3 |
| TMP36 VOUT | A0 / PA0 |
| TMP36 GND | GND |
| MCP9808 VDD | 3V3 |
| MCP9808 SDA | D14 / PB9 |
| MCP9808 SCL | D15 / PB8 |
| MCP9808 GND、A0、A1、A2 | GND |
| MCP9808 ALERT | 不接 |

TMP36 的 TO-92 引脚图在数据手册中是**底视图**，请按手册核对方向。
I²C 使用开漏输出，确认模块带 SDA/SCL 上拉；没有则分别用约 4.7 kΩ 电阻上拉到 3V3。
传感器电源附近放置 0.1 µF 去耦电容，模块已有时可直接使用。

## 构建和上传

温度实验在本目录显式指定环境执行，或选择 PlatformIO 的 `nucleo_f411re` 任务：

```sh
pio run -e nucleo_f411re
pio run -e nucleo_f411re --target upload
pio device monitor -e nucleo_f411re --baud 115200
```

串口输出示例：

```csv
time_ms,mcp9808_c,tmp36_c,mcp_status,tmp_status
1000,24.0625,24.1758,0,0
1250,nan,24.2161,1,0
```

状态：`0` 正常，`1` 读写失败，`2` MCP9808 标识不匹配，`3` 首次转换等待中，`4` 超出 −40～125 °C。
MCP9808 初始化会核对器件标识，设置连续转换及 0.0625 °C 分辨率；读取失败后自动重试初始化。
上电前两次采样可能显示 `nan` / 状态 `3`，随后开始输出温度。

TMP36 每次平均 16 个 ADC 样本，原始换算为 `(ADC平均值 × 3.3 / 4095 − 0.5) × 100`。
**当前启用临时 +50 °C 试验**：在换算后、范围检查前加上 `TMP36_TEST_OFFSET_C = 50.0f`，
因此串口、CSV 和 Python 图表均显示修正值；失败读数仍为 `nan`。
传感器型号尚未确认，此偏移用于试验，不代表完成校准或确认型号。
将 `Core/Src/temperature_sensors.c` 中的 `TMP36_TEST_OFFSET_C` 改回 `0.0f`，重新编译上传即可撤回。
上传后重新开始记录，避免与旧数据混用；原来的 −24.27 °C 预期变为约 25.73 °C。
参考电压默认 3.3 V；实际供电偏差会影响温度，可在 `Core/Src/temperature_sensors.c` 修改
`ADC_REFERENCE_VOLTS` 为实测值。四位小数用于保留 MCP9808 的步进，不代表测量精度。
ADC 无法可靠识别 TMP36 断线：悬空输入也可能输出看似正常的温度，必须核对接线。

## Python 记录和实时绘图

先关闭 PlatformIO 串口监视器，避免两个程序同时占用端口。

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r tools/requirements.txt
python tools/temperature_logger.py --list-ports
python tools/temperature_logger.py --port /dev/cu.usbmodemXXXX
```

把 `/dev/cu.usbmodemXXXX` 换成列出的实际端口。默认记录到 `data/temperature_日期_时间.csv`。
CSV 同时保存电脑 UTC 接收时间、记录开始后的秒数、板上毫秒时间、两路温度及状态。
曲线保留最近 2400 点，CSV 保存全部采样。按 Ctrl+C 或关闭图窗结束。

只记录不绘图、指定文件或定时结束：

```sh
python tools/temperature_logger.py --port /dev/cu.usbmodemXXXX --no-plot --duration 240 --output data/run1.csv
```

程序保护已有 CSV，不覆盖同名文件。读数失败仍写入 CSV，以便区分数据缺失和真实温度变化。

## 本机验证

```sh
python3 -m unittest discover -s test -p 'test_*.py' -v
```

测试需要本机 C 编译器和 pyserial，验证实际 C 温度换算、负温度和状态位，以及分段串口数据的 CSV 记录。
实际传感器、接线和温度响应仍需上传后在板上验证。
