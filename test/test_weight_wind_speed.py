"""Actual C state-machine/protocol checks against a frozen reference LUT.

The independent numeric expectations in the C harness belong to the archived
five-run LUT. Keep it as a fixture so generating a new calibration does not
silently rewrite those expectations or break these algorithm regression checks.
The calibration tool separately verifies and records its active fitted profile.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
LOGGER_PYTHON = ROOT.parent / "WindTunnelLogger/.venv/bin/python"


class WeightWindSpeedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "check_weight_wind_speed"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "test/fixtures/wind_speed_reference"),
            "-I", str(ROOT / "Core/Inc"),
            str(ROOT / "Core/Src/weight_wind_speed.c"),
            str(ROOT / "test/mini_scales_hal/check_weight_wind_speed.c"),
            "-lm", "-o", str(cls.executable),
        ], check=True)

    def check_group(self, group):
        result = subprocess.run([str(self.executable), group], check=True,
                                capture_output=True, text=True)
        self.assertIn("checks passed", result.stdout)

    def test_lut_fixed_delta_points_monotonicity_and_domain(self):
        self.check_group("conversion")

    def test_lut_archived_knots_continuity_midpoints_and_extrapolation(self):
        self.check_group("lut_shape")

    def test_previous_limits_continue_output_and_preserve_fractional_zero(self):
        self.check_group("old_limits")

    def test_debounced_long_press_requires_release_before_retrigger(self):
        self.check_group("button")

    def test_reset_automatically_rezeros_each_baseline_before_any_output(self):
        self.check_group("automatic_reset")

    def test_startup_zero_read_failure_or_instability_stays_silent(self):
        self.check_group("automatic_failures")

    def test_zero_requires_40_actual_spaced_reads_and_rejects_failures(self):
        self.check_group("zero")

    def test_failed_and_stale_reads_recover_without_fake_zero(self):
        self.check_group("recovery")

    def test_interrupted_zero_is_rejected_and_led_states_are_distinct(self):
        self.check_group("status")

    def test_zero_accepts_all_40_reads_with_variable_i2c_completion_times(self):
        self.check_group("read_schedule")

    def test_online_ema_200_hz_step_response_busy_uart_delays_and_tick_wrap(self):
        self.check_group("online_ema")

    def test_repeated_ema_output_does_not_hide_stale_reads_and_recovers(self):
        self.check_group("output_freshness")

    def test_output_path_emits_200_changing_filtered_speeds(self):
        wire = subprocess.check_output([str(self.executable), "ema_wire"])
        values = [float(line) for line in wire.splitlines()]
        self.assertEqual(len(values), 200)
        self.assertTrue(all(a < b for a, b in zip(values, values[1:])))
        self.assertEqual(values[19], 9.654)  # 100 ms endpoint
        self.assertEqual(values[39], 13.601)  # 200 ms endpoint, no upper stop

    def test_online_ema_clears_history_only_on_stale_gap_or_rezero(self):
        self.check_group("ema_gaps")

    def test_high_low_and_returning_reads_keep_ema_and_200_hz_stream(self):
        self.check_group("continuous_output")

    def test_int32_extremes_remain_formattable_and_stream_at_200_hz(self):
        self.check_group("int32_output")

    def test_2000_output_lines_with_100_updates_and_expiry(self):
        self.check_group("stream")

    def test_wrap_busy_and_in_flight_buffer_are_safe(self):
        self.check_group("wrap_busy")

    def test_generated_wire_has_200_three_decimal_speed_lines(self):
        wire = subprocess.check_output([str(self.executable), "wire"])
        lines = wire.splitlines(keepends=True)
        self.assertEqual(len(lines), 200)
        for line in lines:
            self.assertRegex(line, rb"^[0-9]+\.[0-9]{3}\n$")
        expected_speeds = (0.000, 0.000, 0.011, 0.113, 1.127,
                           2.235, 4.484, 7.468, 10.126, 19.686)
        self.assertEqual([float(x) for x in lines],
                         [speed for speed in expected_speeds for _ in range(20)])

    @unittest.skipUnless(LOGGER_PYTHON.exists(), "Official Logger local environment is absent")
    def test_official_logger_accepts_and_preserves_speed_lines(self):
        wire = subprocess.check_output([str(self.executable), "ema_wire"])
        script = r'''
import re, sys
from types import SimpleNamespace
from PySide6 import QtCore
from wind_tunnel_logger.app import StudentSerial, MainWindow, LOG_HEADER, align
app = QtCore.QCoreApplication([])
reader = StudentSerial("offline", 115200, lambda: [])
reader._opened()
received = []
reader.sample.connect(lambda t, v: received.append((t, v)))
class NoWrites:
    def write(self, data):
        raise AssertionError("Pure speed lines must not send commands")
lines = sys.stdin.buffer.read().splitlines(keepends=True)
for i, line in enumerate(lines):
    assert re.fullmatch(rb"[0-9]+\.[0-9]{3}\n", line)
    reader._handle(line.decode("ascii").strip(), i * .005, NoWrites())
assert len(received) == 200 and reader.bad_lines == 0
assert [v for _, v in received] == [float(line) for line in lines]
assert all(0 <= v <= 19.686 for _, v in received)
assert any(v > 12.0 for _, v in received)
proxy = SimpleNamespace(t0=0, min_speed=-2, max_speed=100)
proxy._clip = lambda v: min(max(v, proxy.min_speed), proxy.max_speed)
for t, v in received:
    row = MainWindow._row(proxy, t, "Student speed", "offline", v, False)
    assert float(row[LOG_HEADER.index("value_raw_mps")]) == v
    assert float(row[LOG_HEADER.index("value_clipped_mps")]) == v
paired = align([(t, v) for t, v in received], received, .001)
assert all(g == s for _, g, s in paired)
print("200 speeds accepted by official Logger; 0 bad lines; raw/clipped/aligned preserved")
'''
        result = subprocess.run([str(LOGGER_PYTHON), "-c", script], input=wire,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"200 speeds accepted by official Logger", result.stdout)


if __name__ == "__main__":
    unittest.main()
