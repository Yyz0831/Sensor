#!/usr/bin/env python3
"""Record the weight_i2c stream as CSV without changing board calibration."""

import argparse
from contextlib import ExitStack
import csv
from datetime import datetime, timezone
import math
from pathlib import Path
import re
import sys
import time


HEADER = ("received_utc", "elapsed_s", "time_ms", "label", "raw_adc",
          "weight_g", "weight_status", "communication_status")
RESAMPLED_HEADER = (*HEADER, "output_time_ms", "output_elapsed_s",
                    "source_row", "source_age_ms", "is_held")
STALE_AFTER_MS = 500
SAMPLE = re.compile(
    r"时间=([0-9]+) ms \| 原始ADC=(-?[0-9]+|不可用) \| "
    r"重量=(.*?) \| 状态=(正常|I2C通信失败)")
GRAMS = re.compile(r"[+-]?[0-9]+(?:\.[0-9]+)? g")


def parse_sample(line):
    """Keep signed raw counts and failed samples; reject incomplete/corrupt rows."""
    match = SAMPLE.fullmatch(line.strip())
    if match is None:
        return None
    tick, raw, weight, status = match.groups()
    tick = int(tick)
    if not 0 <= tick <= 0xFFFFFFFF:
        return None
    if status == "I2C通信失败":
        if raw != "不可用" or weight != "nan g":
            return None
        return tick, "", "nan", "不可用", status
    if raw == "不可用":
        return None
    raw = int(raw)
    if not -(2 ** 31) <= raw < 2 ** 31:
        return None
    if weight in ("待去皮", "未校准"):
        return tick, raw, "", weight, status
    if GRAMS.fullmatch(weight) is None:
        return None
    grams = weight[:-2]
    if not math.isfinite(float(grams)):
        return None
    return tick, raw, grams, "正常", status


def record(connection, handle, label, duration):
    writer = csv.writer(handle)
    writer.writerow(HEADER)
    handle.flush()
    started = time.monotonic()
    last_received = last_warning = started
    pending = bytearray()
    discard = False
    count = 0
    # The caller owns the count so even an interrupted/failed read has a summary.
    while duration is None or time.monotonic() - started < duration:
        chunk = connection.read(max(1, min(connection.in_waiting, 4096)))
        pending.extend(chunk)
        while b"\n" in pending:
            raw_line, _, remainder = pending.partition(b"\n")
            pending = bytearray(remainder)
            if discard or len(raw_line) > 4096:
                discard = False
                continue
            try:
                sample = parse_sample(raw_line.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                sample = None
            if sample is None:
                continue
            tick, adc, grams, weight_status, status = sample
            now = time.monotonic()
            elapsed = now - started
            received = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
            writer.writerow((received, f"{elapsed:.3f}", tick, label, adc,
                             grams, weight_status, status))
            handle.flush()
            count += 1
            last_received = now
            print(f"{elapsed:8.2f} s | 原始ADC={adc if adc != '' else '不可用'} | "
                  f"重量={grams + ' g' if grams else weight_status} | 状态={status}",
                  flush=True)
            yield count
        if len(pending) > 4096:
            pending.clear()
            discard = True
        now = time.monotonic()
        if now - last_received >= 3 and now - last_warning >= 3:
            print("连续3秒未收到有效样本；请检查连接、weight_i2c固件及115200波特率。",
                  file=sys.stderr, flush=True)
            last_warning = now


def resample_csv(source, destination):
    """200 Hz zero-order hold on board time; never invent independent readings.

    Original columns refer to the source row. New output_* columns describe the
    uniform 5 ms grid. Only cover the observed time span, with no extrapolation.
    """
    reader = csv.DictReader(source)
    writer = csv.DictWriter(destination, fieldnames=RESAMPLED_HEADER)
    writer.writeheader()
    count = 0
    if reader.fieldnames is None:
        destination.flush()
        return 0
    if tuple(reader.fieldnames) != HEADER:
        raise ValueError("原始CSV字段不匹配，无法生成200 Hz文件")
    previous = None
    previous_tick = previous_time = origin = next_time = source_row = 0

    def emit(row, row_time, row_number, grid_time):
        age = grid_time - row_time
        output_row = dict(row)
        output_row.update(output_time_ms=grid_time,
                          output_elapsed_s=f"{(grid_time - origin) / 1000:.3f}",
                          source_row=row_number, source_age_ms=age,
                          is_held=int(age > 0))
        # Do not label a long serial gap as a successful measurement.
        if age >= STALE_AFTER_MS:
            output_row.update(raw_adc="", weight_g="nan", weight_status="不可用",
                              communication_status="数据过期")
        writer.writerow(output_row)

    for row_number, row in enumerate(reader, start=1):
        tick = int(row["time_ms"])
        if not 0 <= tick <= 0xFFFFFFFF:
            raise ValueError("原始CSV包含无效板上时间")
        if previous is None:
            origin = next_time = current_time = tick
        else:
            delta = (tick - previous_tick) & 0xFFFFFFFF
            if delta > 0x7FFFFFFF:
                raise ValueError("检测到板上时间倒退/复位，请按复位前后分别记录；原始CSV已保留")
            current_time = previous_time + delta
            while next_time < current_time:
                emit(previous, previous_time, source_row, next_time)
                count += 1
                next_time += 5
        previous = row
        previous_tick = tick
        previous_time = current_time
        source_row = row_number
    if previous is not None and next_time == previous_time:
        emit(previous, previous_time, source_row, next_time)
        count += 1
    destination.flush()
    return count


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="例如 /dev/cu.usbmodem1103")
    parser.add_argument("--list-ports", action="store_true", help="列出串口后退出")
    parser.add_argument("--label", default="unlabelled", help="板子/测试轮次标识")
    parser.add_argument("--duration", type=float, help="采集秒数；默认直到Ctrl+C")
    parser.add_argument("--output", type=Path, help="CSV路径；不覆盖已有文件")
    parser.add_argument("--output-hz", type=int, choices=(200,),
                        help="结束后生成200 Hz保持值CSV，同时保留_raw.csv原始记录")
    args = parser.parse_args(argv)
    if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
        parser.error("--duration必须是有限的正数，单位秒")
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        parser.exit(1, "请使用项目 .venv/bin/python，或安装 tools/requirements.txt。\n")
    if args.list_ports:
        for port in list_ports.comports():
            print(f"{port.device}\t{port.description}")
        return 0
    if not args.port:
        parser.error("请指定--port，或使用--list-ports查看串口")

    output = args.output or Path("data") / datetime.now().strftime("weight_%Y%m%d_%H%M%S.csv")
    raw_output = (output.with_name(output.stem + "_raw" + output.suffix)
                  if args.output_hz else output)
    count = 0
    resampled_count = None
    created = False
    result = 0
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        if args.output_hz and raw_output.exists():
            raise FileExistsError(f"原始记录文件已存在：{raw_output}")
        with ExitStack() as stack:
            output_handle = stack.enter_context(output.open("x", newline="", encoding="utf-8"))
            created = True
            handle = (stack.enter_context(raw_output.open("x", newline="", encoding="utf-8"))
                      if args.output_hz else output_handle)
            # Match weight_control: set control lines before opening, with no
            # reset, tare, cal, or other serial writes. / 不发送任何板上命令。
            connection = serial.Serial(port=None, baudrate=115200, timeout=0.05,
                                       exclusive=True)
            connection.dtr = False
            connection.rts = False
            connection.port = args.port
            try:
                with connection:
                    print(f"记录 {args.port} → {raw_output} | 标识={args.label} | Ctrl+C停止",
                          flush=True)
                    if args.output_hz:
                        print("结束后生成200 Hz保持值CSV；新测量频率仍由板上固件决定。", flush=True)
                    for count in record(connection, handle, args.label, args.duration):
                        pass
            except KeyboardInterrupt:
                pass
            except (serial.SerialException, OSError) as error:
                print(f"串口记录中止：{error}\n请关闭占用该端口的其他工具，再检查连接。",
                      file=sys.stderr)
                result = 1
            if args.output_hz:
                handle.flush()
                with raw_output.open(newline="", encoding="utf-8") as source:
                    resampled_count = resample_csv(source, output_handle)
    except KeyboardInterrupt:
        pass
    except (serial.SerialException, OSError, ValueError) as error:
        print(f"记录未完成：{error}\n请检查路径，并关闭占用该端口的其他串口工具。",
              file=sys.stderr)
        result = 1
    finally:
        if created:
            print(f"已保存 {count} 条样本：{raw_output}", flush=True)
            if resampled_count is not None:
                print(f"已生成 {resampled_count} 行200 Hz保持值数据：{output}", flush=True)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
