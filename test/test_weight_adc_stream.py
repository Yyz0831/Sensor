"""Actual C wire formatter/scheduler and installed official Logger acceptance."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LOGGER_PYTHON = ROOT.parent / "WindTunnelLogger/.venv/bin/python"


class WeightAdcStreamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "check_weight_adc_stream"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "Core/Inc"),
            str(ROOT / "Core/Src/weight_adc_stream.c"),
            str(ROOT / "test/mini_scales_hal/check_weight_adc_stream.c"),
            "-o", str(cls.executable),
        ], check=True)

    def check_group(self, group):
        result = subprocess.run([str(self.executable), group], check=True,
                                capture_output=True, text=True)
        self.assertIn("checks passed", result.stdout)

    def test_integer_only_full_int32_protocol_and_uart_budget(self):
        self.check_group("protocol")

    def test_2000_lines_in_10_seconds_with_100_source_reads(self):
        self.check_group("timing")

    def test_startup_failure_stale_silence_and_recovery(self):
        self.check_group("failures")

    def test_tick_wrap_uart_busy_and_buffer_boundaries(self):
        self.check_group("wrap_busy")

    def test_wire_fixture_is_200_complete_integer_lines(self):
        wire = subprocess.check_output([str(self.executable), "wire"])
        lines = wire.splitlines(keepends=True)
        self.assertEqual(len(lines), 200)
        for line in lines:
            self.assertRegex(line, rb"^-?[0-9]+\n$")
        self.assertEqual([int(line) for line in lines[:20]], [8769386] * 20)

    @unittest.skipUnless(LOGGER_PYTHON.exists(), "Official Logger's optional local environment is absent")
    def test_official_logger_accepts_adc_without_clipping_saved_raw_columns(self):
        wire = subprocess.check_output([str(self.executable), "wire"])
        # Use the installed package as-is. No GUI, USB, settings, or files are changed.
        script = r'''
import sys
from types import SimpleNamespace
from PySide6 import QtCore
from wind_tunnel_logger.app import StudentSerial, MainWindow, LOG_HEADER, align
app = QtCore.QCoreApplication([])
receiver = StudentSerial("offline", 115200, lambda: [])
receiver._opened()
received = []
receiver.sample.connect(lambda t, value: received.append((t, value)))
class NoWrites:
    def write(self, data):
        raise AssertionError("Pure ADC input must not trigger board commands")
lines = sys.stdin.buffer.read().splitlines()
for i, line in enumerate(lines):
    receiver._handle(line.decode("ascii"), i * 0.005, NoWrites())
assert receiver.bad_lines == 0 and len(received) == 200
assert [v for _, v in received] == [float(line) for line in lines]
proxy = SimpleNamespace(t0=0, min_speed=-2.0, max_speed=100.0)
proxy._clip = lambda v: min(max(v, proxy.min_speed), proxy.max_speed)
raw_index = LOG_HEADER.index("value_raw_mps")
clip_index = LOG_HEADER.index("value_clipped_mps")
for t, value in received:
    row = MainWindow._row(proxy, t, "ADC counts", "offline", value, False)
    assert float(row[raw_index]) == value
    assert float(row[clip_index]) == proxy._clip(value)
paired = align([(t, 3.0) for t, _ in received], received, 0.001)
assert len(paired) == 200
assert [s for _, _, s in paired] == [v for _, v in received]
print("200 official Logger samples, 0 bad lines, raw/aligned ADC preserved")
'''
        result = subprocess.run([str(LOGGER_PYTHON), "-c", script], input=wire,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"200 official Logger samples, 0 bad lines", result.stdout)


if __name__ == "__main__":
    unittest.main()
