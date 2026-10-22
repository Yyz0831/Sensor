#!/usr/bin/env python3
"""Send tare/cal commands without typing into a scrolling serial monitor."""

import argparse
import math
import sys
import time


def wait_for(connection, expected, timeout, show_messages=False):
    expected_messages = (expected,) if isinstance(expected, str) else expected
    deadline = time.monotonic() + timeout
    pending = bytearray()
    while time.monotonic() < deadline:
        pending.extend(connection.read(max(1, min(connection.in_waiting, 4096))))
        while b"\n" in pending:
            raw, _, rest = pending.partition(b"\n")
            pending = bytearray(rest)
            line = raw.decode("utf-8", errors="replace").strip()
            if show_messages and line.startswith("#"):
                print(line, flush=True)
            if any(message in line for message in expected_messages):
                return line
            if show_messages and line.startswith("#") and any(
                marker in line for marker in ("失败", "中止", "无效命令", "请先空载", "正在采集", "接收溢出")
            ):
                raise RuntimeError(line)
        if len(pending) > 4096:
            pending.clear()
    raise RuntimeError("未收到预期回复；请检查端口、weight_i2c固件和连接。")


def run_command(connection, command):
    # A leading newline clears a partial command in the board's line buffer.
    connection.reset_input_buffer()
    connection.write(b"\nhelp\n")
    connection.flush()
    # Support the original RAM-only firmware and the newer preset-gain version.
    wait_for(connection, ("协议=weight_i2c_v1", "校准仅保存在RAM"), 3)
    connection.write((command + "\n").encode("ascii"))
    connection.flush()
    expected = "去皮完成" if command == "tare" else "校准完成"
    return wait_for(connection, expected, 8, show_messages=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="例如 /dev/cu.usbmodem1103")
    subcommands = parser.add_subparsers(dest="operation", required=True)
    subcommands.add_parser("tare", help="空载稳定后执行去皮")
    cal = subcommands.add_parser("cal", help="已放好已知质量的砝码后执行校准")
    cal.add_argument("mass_g", type=float, help="砝码实际克数，例如 50")
    args = parser.parse_args(argv)
    if args.operation == "cal" and (not math.isfinite(args.mass_g) or args.mass_g <= 0):
        parser.error("砝码质量必须是有限正数，单位g")
    try:
        import serial
    except ImportError:
        parser.exit(1, "请使用项目 .venv/bin/python，或安装 tools/requirements.txt。\n")
    command = "tare" if args.operation == "tare" else f"cal {args.mass_g:.12g}"
    try:
        # Set control lines before opening; do not deliberately reset the MCU,
        # whose zero/gain are RAM-only and must survive between invocations.
        connection = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                                   exclusive=True)
        connection.dtr = False
        connection.rts = False
        connection.port = args.port
        with connection:
            print(f"发送 {command}；请保持载荷稳定约4秒。", flush=True)
            run_command(connection, command)
    except (serial.SerialException, OSError, RuntimeError) as error:
        print(f"操作未完成：{error}\n先关闭其他串口监视器，再重试。", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("等待已取消；已发送的板上采集可能仍会完成。", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
