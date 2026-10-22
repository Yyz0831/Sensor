"""Compile the real STM32 Mini Scales driver against a simulated I2C device."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MiniScalesTests(unittest.TestCase):
    def test_protocol_and_error_handling(self):
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "check_mini_scales"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "test/mini_scales_hal"),
                "-I", str(ROOT / "Core/Inc"),
                str(ROOT / "Core/Src/mini_scales.c"),
                str(ROOT / "test/mini_scales_hal/check_driver.c"),
                "-lm", "-o", str(executable),
            ], check=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
            self.assertIn("tare passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
