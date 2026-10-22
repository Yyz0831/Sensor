"""Host checks: python3 -m unittest discover -s test -p 'test_*.py' -v."""

import contextlib
import csv
import ctypes
import importlib.util
import io
import math
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("temperature_logger", ROOT / "tools/temperature_logger.py")
logger = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logger)


class ConversionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        library = Path(cls.folder.name) / "conversion.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "Core/Inc"),
                        str(ROOT / "Core/Src/temperature_conversion.c"),
                        "-o", str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.library.MCP9808_DecodeTemperature.argtypes = [ctypes.c_uint16]
        cls.library.MCP9808_DecodeTemperature.restype = ctypes.c_float
        cls.library.TMP36_ConvertTemperature.argtypes = [ctypes.c_float, ctypes.c_float]
        cls.library.TMP36_ConvertTemperature.restype = ctypes.c_float

    def test_mcp_temperature_sign_fraction_and_alert_flags(self):
        for raw, expected in [(0x0000, 0), (0x0190, 25), (0x0191, 25.0625),
                              (0x1FFF, -0.0625), (0x1F60, -10), (0x1D80, -40),
                              (0x07D0, 125), (0xE190, 25), (0xFF60, -10)]:
            with self.subTest(raw=hex(raw)):
                self.assertEqual(self.library.MCP9808_DecodeTemperature(raw), expected)

    def test_tmp36_datasheet_voltages_and_reference_calibration(self):
        # TMP36: 100 mV at -40 C, 500 mV at 0 C, 750 mV at 25 C.
        for reference in (3.3, 3.25):
            for voltage, expected in [(0.1, -40), (0.5, 0), (0.75, 25), (1.75, 125)]:
                with self.subTest(reference=reference, voltage=voltage):
                    counts = voltage / reference * 4095
                    value = self.library.TMP36_ConvertTemperature(counts, reference)
                    self.assertAlmostEqual(value, expected, places=4)


class FakeSerial:
    """Fragmented byte stream with warm-up, corrupt input, and a dropped sensor."""
    def __init__(self, *args, **kwargs):
        self.pending = bytearray(
            b"time_ms,mcp9808_c,tmp36_c,mcp_status,tmp_status\r\n"
            b"250,nan,25.0000,3,0\r\n"
            b"broken line\n"
            b"750,25.0625,24.9000,0,0\r\n"
            b"1000,nan,24.8000,1,0\r\n")

    @property
    def in_waiting(self):
        return min(len(self.pending), 7)

    def read(self, count):
        if not self.pending:
            time.sleep(0.01)
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass


class LoggerTests(unittest.TestCase):
    def test_failed_sensor_rows_are_retained(self):
        sample = logger.parse_sample("1000,nan,-10.0625,1,0\r\n")
        self.assertTrue(math.isnan(sample[1]))
        self.assertEqual(sample[2:], (-10.0625, 1, 0))

    def test_invalid_data_is_rejected(self):
        for line in ["header", "1000,25,24,0", "-1,25,24,0,0",
                     "1000,inf,24,0,0", "1000,nan,24,0,0",
                     "1000,25,24,99,0", "1000,25,24,1,0"]:
            with self.subTest(line=line):
                self.assertIsNone(logger.parse_sample(line))

    def test_fragmented_serial_stream_is_logged_and_csv_is_protected(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / "run.csv"
            arguments = ["--port", "fake", "--no-plot", "--duration", "0.1",
                         "--output", str(output)]
            with patch("serial.Serial", FakeSerial), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(logger.main(arguments), 0)
                original = output.read_bytes()
                with output.open(newline="") as handle:
                    rows = list(csv.DictReader(handle))
                self.assertEqual(len(rows), 3)
                self.assertEqual(rows[1]["mcp9808_c"], "25.0625")
                self.assertEqual(rows[2]["mcp_status"], "1")
                self.assertEqual(rows[2]["mcp9808_c"], "nan")
                self.assertTrue(all(row["received_utc"].endswith("+00:00") for row in rows))
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(logger.main(arguments), 1)
                self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
