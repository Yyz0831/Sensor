"""Offline serial/CSV checks; no physical board is needed."""

import contextlib
import csv
import importlib.util
import io
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

import serial


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("weight_logger", ROOT / "tools/weight_logger.py")
logger = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(logger)


def sample(tick=1000, adc="123456", weight="待去皮", status="正常"):
    return f"时间={tick} ms | 原始ADC={adc} | 重量={weight} | 状态={status}"


def raw_csv(samples):
    handle = io.StringIO()
    writer = csv.writer(handle)
    writer.writerow(logger.HEADER)
    for index, (tick, adc, grams, weight_status, status) in enumerate(samples):
        writer.writerow(("2026-10-22T12:00:00.000+00:00", f"{index * 0.1:.3f}",
                         tick, "plate", adc, grams, weight_status, status))
    handle.seek(0)
    return handle


class FakeSerial:
    def __init__(self, data=b"", ending=KeyboardInterrupt):
        self.pending = bytearray(data)
        self.ending = ending
        self.closed = False
        self.entered = False

    def __call__(self, **kwargs):
        self.options = kwargs
        return self

    @property
    def in_waiting(self):
        return min(7, len(self.pending))

    def read(self, count):
        if not self.pending:
            if self.ending is not None:
                raise self.ending()
            time.sleep(0.001)
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result

    def write(self, data):
        raise AssertionError("The logger must not send commands")

    def __enter__(self):
        assert self.dtr is False and self.rts is False
        self.entered = True
        return self

    def __exit__(self, *args):
        self.closed = True


class WeightLoggerTests(unittest.TestCase):
    def test_all_firmware_weight_states_and_signed_adc(self):
        self.assertEqual(logger.parse_sample(sample()), (1000, 123456, "", "待去皮", "正常"))
        self.assertEqual(logger.parse_sample(sample(adc="-123456", weight="未校准")),
                         (1000, -123456, "", "未校准", "正常"))
        self.assertEqual(logger.parse_sample(sample(weight="-2.50 g")),
                         (1000, 123456, "-2.50", "正常", "正常"))
        self.assertEqual(logger.parse_sample(sample(adc="0", weight="0.00 g")),
                         (1000, 0, "0.00", "正常", "正常"))
        self.assertEqual(logger.parse_sample(sample(adc="不可用", weight="nan g", status="I2C通信失败")),
                         (1000, "", "nan", "不可用", "I2C通信失败"))
        for adc in (str(-(2 ** 31)), str(2 ** 31 - 1)):
            self.assertEqual(logger.parse_sample(sample(tick=0xFFFFFFFF, adc=adc))[1], int(adc))

    def test_corrupt_or_inconsistent_rows_are_rejected(self):
        lines = ["# TAL220B + Unit Weight-I2C", "24250,nan,115.8727,1,0",
                 sample()[:-1], sample(tick=-1), sample(tick=2 ** 32),
                 sample(adc=str(2 ** 31)), sample(adc=str(-(2 ** 31) - 1)),
                 sample(adc="不可用"), sample(weight="nan g"), sample(weight="inf g"),
                 sample(weight="1e99 g"), sample(weight="999"), sample(status="未知"),
                 sample(status="I2C通信失败"),
                 sample(adc="不可用", weight="待去皮", status="I2C通信失败")]
        for line in lines:
            with self.subTest(line=line):
                self.assertIsNone(logger.parse_sample(line))

    def test_fragmented_utf8_failure_recovery_and_interrupt_save_csv(self):
        lines = ["# 启动提示", sample(adc="-10"), sample(tick=1250, weight="未校准"),
                 "broken line", sample(tick=1500, adc="不可用", weight="nan g", status="I2C通信失败"),
                 sample(tick=1750, adc="987654", weight="12.30 g")]
        data = ("\r\n".join(lines) + "\r\n").encode("utf-8")
        connection = FakeSerial(data)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "nested/run.csv"
            stdout = io.StringIO()
            with patch("serial.Serial", connection), contextlib.redirect_stdout(stdout):
                result = logger.main(["--port", "fake", "--label", "板100x100,第1轮",
                                      "--output", str(output)])
            self.assertEqual(result, 0)
            self.assertTrue(connection.closed)
            self.assertEqual(connection.options, dict(port=None, baudrate=115200,
                                                      timeout=0.05, exclusive=True))
            with output.open(newline="", encoding="utf-8") as handle:
                reader = csv.DictReader(handle)
                rows = list(reader)
                self.assertEqual(reader.fieldnames, list(logger.HEADER))
            self.assertEqual(len(rows), 4)
            self.assertEqual([r["raw_adc"] for r in rows], ["-10", "123456", "", "987654"])
            self.assertEqual([r["weight_g"] for r in rows], ["", "", "nan", "12.30"])
            self.assertEqual([r["weight_status"] for r in rows],
                             ["待去皮", "未校准", "不可用", "正常"])
            self.assertEqual(rows[2]["communication_status"], "I2C通信失败")
            self.assertTrue(all(r["label"] == "板100x100,第1轮" for r in rows))
            self.assertTrue(all(r["received_utc"].endswith("+00:00") for r in rows))
            elapsed = [float(r["elapsed_s"]) for r in rows]
            self.assertEqual(elapsed, sorted(elapsed))
            self.assertIn("已保存 4 条样本", stdout.getvalue())

    def test_existing_file_is_untouched_and_serial_is_not_opened(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "run.csv"
            output.write_bytes(b"existing experiment\n")
            with patch("serial.Serial") as open_serial, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(logger.main(["--port", "fake", "--output", str(output)]), 1)
            open_serial.assert_not_called()
            self.assertEqual(output.read_bytes(), b"existing experiment\n")

    def test_disconnect_preserves_already_flushed_samples(self):
        connection = FakeSerial((sample() + "\n").encode(), serial.SerialException)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "run.csv"
            with patch("serial.Serial", connection), contextlib.redirect_stdout(io.StringIO()) as stdout, \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(logger.main(["--port", "fake", "--output", str(output)]), 1)
            self.assertTrue(connection.closed)
            with output.open(newline="") as handle:
                self.assertEqual(len(list(csv.DictReader(handle))), 1)
            self.assertIn("已保存 1 条样本", stdout.getvalue())

    def test_duration_stops_and_default_label_is_saved(self):
        connection = FakeSerial((sample() + "\n").encode(), ending=None)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "run.csv"
            with patch("serial.Serial", connection), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(logger.main(["--port", "fake", "--duration", "0.03",
                                              "--output", str(output)]), 0)
            self.assertTrue(connection.closed)
            with output.open(newline="") as handle:
                self.assertEqual(list(csv.DictReader(handle))[0]["label"], "unlabelled")

    def test_silence_warns_without_inventing_samples(self):
        handle = io.StringIO()
        connection = FakeSerial(ending=None)
        with patch.object(logger.time, "monotonic", side_effect=range(20)), \
                contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.assertEqual(list(logger.record(connection, handle, "plate", 6)), [])
        self.assertIn("连续3秒", stderr.getvalue())
        self.assertEqual(list(csv.reader(io.StringIO(handle.getvalue()))), [list(logger.HEADER)])

    def test_invalid_utf8_and_overlong_input_recover_at_next_line(self):
        data = b"\xff\n" + b"x" * 5000 + b"\n" + (sample() + "\n").encode()
        handle = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(KeyboardInterrupt):
            list(logger.record(FakeSerial(data), handle, "plate", None))
        self.assertEqual(len(list(csv.DictReader(io.StringIO(handle.getvalue())))), 1)

    def test_invalid_duration_is_rejected_before_open(self):
        for duration in ("0", "-1", "nan", "inf"):
            with self.subTest(duration=duration), patch("serial.Serial") as open_serial, \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as result:
                logger.main(["--port", "fake", "--duration", duration])
            self.assertEqual(result.exception.code, 2)
            open_serial.assert_not_called()

    def test_port_listing_needs_no_port_or_file(self):
        class Port:
            device = "/dev/cu.fake"
            description = "Fake ST-Link"
        with patch("serial.tools.list_ports.comports", return_value=[Port()]), \
                patch("serial.Serial") as open_serial, contextlib.redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(logger.main(["--list-ports"]), 0)
        open_serial.assert_not_called()
        self.assertIn("/dev/cu.fake\tFake ST-Link", stdout.getvalue())


class ResamplingTests(unittest.TestCase):
    def resample(self, samples):
        destination = io.StringIO()
        count = logger.resample_csv(raw_csv(samples), destination)
        rows = list(csv.DictReader(io.StringIO(destination.getvalue())))
        self.assertEqual(count, len(rows))
        return rows

    def test_10hz_sources_make_uniform_200hz_grid_without_future_values(self):
        samples = [(1000 + i * 100, i, "", "待去皮", "正常") for i in range(11)]
        rows = self.resample(samples)
        self.assertEqual(len(rows), 201)  # Inclusive endpoints over exactly 1 s.
        self.assertEqual([int(r["output_time_ms"]) for r in rows], list(range(1000, 2001, 5)))
        self.assertEqual([r["output_elapsed_s"] for r in rows[:3]], ["0.000", "0.005", "0.010"])
        for i, row in enumerate(rows):
            self.assertEqual(int(row["raw_adc"]), i // 20)
            self.assertEqual(int(row["source_row"]), i // 20 + 1)
            self.assertEqual(int(row["time_ms"]), 1000 + (i // 20) * 100)
            self.assertEqual(int(row["source_age_ms"]), i % 20 * 5)
            self.assertEqual(int(row["is_held"]), int(i % 20 != 0))

    def test_unaligned_source_times_hold_previous_without_interpolation(self):
        rows = self.resample([(1000, -10, "-1.00", "正常", "正常"),
                              (1101, 99, "9.90", "正常", "正常"),
                              (1202, 200, "20.00", "正常", "正常")])
        self.assertEqual(rows[20]["output_time_ms"], "1100")
        self.assertEqual(rows[20]["raw_adc"], "-10")
        self.assertEqual(rows[21]["output_time_ms"], "1105")
        self.assertEqual(rows[21]["raw_adc"], "99")
        self.assertEqual(rows[21]["source_age_ms"], "4")
        self.assertEqual(rows[-1]["output_time_ms"], "1200")
        self.assertEqual(rows[-1]["raw_adc"], "99")  # No invented point beyond last source.

    def test_failure_and_recovery_are_held_without_zero_filling(self):
        rows = self.resample([(0, 100, "", "未校准", "正常"),
                              (100, "", "nan", "不可用", "I2C通信失败"),
                              (200, -50, "", "待去皮", "正常")])
        for row in rows[20:40]:
            self.assertEqual(row["raw_adc"], "")
            self.assertEqual(row["weight_g"], "nan")
            self.assertEqual(row["communication_status"], "I2C通信失败")
        self.assertEqual(rows[-1]["raw_adc"], "-50")
        self.assertEqual(rows[-1]["communication_status"], "正常")

    def test_long_gap_is_marked_stale_then_recovers(self):
        rows = self.resample([(0, 123, "", "待去皮", "正常"),
                              (1000, 456, "", "待去皮", "正常")])
        self.assertEqual(rows[99]["raw_adc"], "123")
        for row in rows[100:200]:
            self.assertEqual(row["raw_adc"], "")
            self.assertEqual(row["communication_status"], "数据过期")
            self.assertEqual(row["weight_g"], "nan")
        self.assertEqual(rows[-1]["raw_adc"], "456")

    def test_wraparound_is_unwrapped_and_reset_is_rejected(self):
        rows = self.resample([(0xFFFFFFF0, 1, "", "待去皮", "正常"),
                              (84, 2, "", "待去皮", "正常")])
        self.assertEqual(len(rows), 21)
        self.assertEqual(int(rows[-1]["output_time_ms"]), 0xFFFFFFF0 + 100)
        self.assertEqual(rows[-1]["time_ms"], "84")
        with self.assertRaisesRegex(ValueError, "复位"):
            self.resample([(1000, 1, "", "待去皮", "正常"),
                           (100, 2, "", "待去皮", "正常")])

    def test_empty_input_and_single_sample_do_not_extrapolate(self):
        self.assertEqual(self.resample([]), [])
        self.assertEqual(len(self.resample([(1234, 1, "", "待去皮", "正常")])), 1)
        with self.assertRaisesRegex(ValueError, "字段"):
            logger.resample_csv(io.StringIO("other,fields\n1,2\n"), io.StringIO())

    def test_cli_keeps_raw_and_finalizes_200hz_after_interrupt_or_disconnect(self):
        data = (sample(tick=1000, adc="-10") + "\n" + sample(tick=1100, adc="20") + "\n").encode()
        for ending, expected in [(KeyboardInterrupt, 0), (serial.SerialException, 1)]:
            with self.subTest(ending=ending), tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / "plate.csv"
                connection = FakeSerial(data, ending=ending)
                with patch("serial.Serial", connection), contextlib.redirect_stdout(io.StringIO()) as stdout, \
                        contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(logger.main(["--port", "fake", "--output-hz", "200",
                                                  "--output", str(output)]), expected)
                with output.with_name("plate_raw.csv").open(newline="") as handle:
                    raw = list(csv.DictReader(handle))
                with output.open(newline="") as handle:
                    final = list(csv.DictReader(handle))
                self.assertEqual(len(raw), 2)
                self.assertEqual([r["raw_adc"] for r in raw], ["-10", "20"])
                self.assertEqual(len(final), 21)
                self.assertIn("已生成 21 行200 Hz", stdout.getvalue())

    def test_existing_raw_companion_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "plate.csv"
            raw_output = output.with_name("plate_raw.csv")
            raw_output.write_bytes(b"original\n")
            with patch("serial.Serial") as open_serial, contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(logger.main(["--port", "fake", "--output-hz", "200",
                                              "--output", str(output)]), 1)
            self.assertEqual(raw_output.read_bytes(), b"original\n")
            self.assertFalse(output.exists())
            open_serial.assert_not_called()


if __name__ == "__main__":
    unittest.main()
