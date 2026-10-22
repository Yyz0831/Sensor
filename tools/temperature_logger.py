#!/usr/bin/env python3
"""Read the Lab 1 temperature stream, save CSV, and optionally plot it."""

import argparse
import csv
from collections import deque
from datetime import datetime, timezone
import math
from pathlib import Path
import sys
import time

HEADER = ("time_ms", "mcp9808_c", "tmp36_c", "mcp_status", "tmp_status")
STATUS = {0: "OK", 1: "read failed", 2: "wrong sensor ID",
          3: "warming up", 4: "outside -40..125 C"}


def parse_sample(line):
    """Ignore headers, partial lines, and corrupt rows; retain failed readings."""
    fields = line.strip().split(",")
    if len(fields) != 5:
        return None
    try:
        tick = int(fields[0])
        mcp, tmp = float(fields[1]), float(fields[2])
        mcp_status, tmp_status = int(fields[3]), int(fields[4])
    except ValueError:
        return None
    if not 0 <= tick <= 0xFFFFFFFF:
        return None
    for value, status in ((mcp, mcp_status), (tmp, tmp_status)):
        if status not in STATUS:
            return None
        if status == 0 and (not math.isfinite(value) or not -40 <= value <= 125):
            return None
        if status != 0 and not math.isnan(value):
            return None
    return tick, mcp, tmp, mcp_status, tmp_status


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Serial device, e.g. /dev/cu.usbmodemXXXX")
    parser.add_argument("--list-ports", action="store_true")
    parser.add_argument("--output", type=Path, help="New CSV file (existing files are protected)")
    parser.add_argument("--duration", type=float, help="Stop after this many seconds")
    parser.add_argument("--no-plot", action="store_true", help="Log and print without a plot")
    args = parser.parse_args(argv)
    if args.duration is not None and (not math.isfinite(args.duration) or args.duration <= 0):
        parser.error("--duration must be a positive, finite number")
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        parser.exit(1, "Install dependencies: python3 -m pip install -r tools/requirements.txt\n")
    if args.list_ports:
        for port in list_ports.comports():
            print(f"{port.device}\t{port.description}")
        return 0
    if not args.port:
        parser.error("supply --port or use --list-ports")

    plt = None
    if not args.no_plot:
        try:
            import matplotlib.pyplot as plt
        except ImportError:
            parser.exit(1, "Install matplotlib or run with --no-plot.\n")
    output = args.output or Path("data") / datetime.now().strftime("temperature_%Y%m%d_%H%M%S.csv")
    output.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    try:
        # Short reads keep the plot responsive even when the board is silent.
        with serial.Serial(args.port, 115200, timeout=0.05) as connection:
            with output.open("x", newline="", encoding="utf-8") as handle:
                writer = csv.writer(handle)
                writer.writerow(("received_utc", "elapsed_s", *HEADER))
                handle.flush()
                print(f"Logging {args.port} at 115200 baud to {output}")
                print("Press Ctrl+C or close the plot to stop.")
                if plt is not None:
                    plt.ion()
                    figure, axis = plt.subplots()
                    mcp_line, = axis.plot([], [], label="MCP9808")
                    tmp_line, = axis.plot([], [], label="TMP36")
                    axis.set(xlabel="Elapsed time (s)", ylabel="Temperature (C)")
                    axis.grid(True)
                    axis.legend()
                    plt.show(block=False)
                times, mcp_values, tmp_values = (deque(maxlen=2400) for _ in range(3))
                started = time.monotonic()
                last_received = started
                last_warning = started
                pending = bytearray()
                while args.duration is None or time.monotonic() - started < args.duration:
                    if plt is not None and not plt.fignum_exists(figure.number):
                        break
                    pending.extend(connection.read(max(1, min(connection.in_waiting, 4096))))
                    while b"\n" in pending:
                        raw, _, remainder = pending.partition(b"\n")
                        pending = bytearray(remainder)
                        sample = parse_sample(raw.decode("ascii", errors="replace"))
                        if sample is None:
                            continue
                        now = time.monotonic()
                        elapsed = now - started
                        last_received = now
                        received = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
                        writer.writerow((received, f"{elapsed:.3f}", *sample))
                        handle.flush()
                        count += 1
                        _, mcp, tmp, mcp_status, tmp_status = sample
                        print(f"{elapsed:7.2f}s  MCP9808={mcp:8.4f} C [{STATUS[mcp_status]}]  "
                              f"TMP36={tmp:8.4f} C [{STATUS[tmp_status]}]")
                        times.append(elapsed)
                        mcp_values.append(mcp)
                        tmp_values.append(tmp)
                    if len(pending) > 4096:
                        pending.clear()
                    now = time.monotonic()
                    if now - last_received > 3 and now - last_warning > 3:
                        print("No valid samples: check firmware, port, and 115200 baud.", file=sys.stderr)
                        last_warning = now
                    if plt is not None:
                        mcp_line.set_data(times, mcp_values)
                        tmp_line.set_data(times, tmp_values)
                        axis.relim()
                        axis.autoscale_view()
                        plt.pause(0.001)
    except KeyboardInterrupt:
        pass
    except (serial.SerialException, OSError) as error:
        print(f"Cannot log: {error}", file=sys.stderr)
        return 1
    finally:
        if plt is not None:
            plt.close("all")
    print(f"Saved {count} samples to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
