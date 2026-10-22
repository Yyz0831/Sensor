"""Host checks of the real U180 driver, RAM calibration, and command parser."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WeightI2CTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.folder.cleanup)
        cls.executable = Path(cls.folder.name) / "check_weight_i2c"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "test/mini_scales_hal"),
            "-I", str(ROOT / "Core/Inc"),
            *(str(ROOT / "Core/Src" / name) for name in
              ("weight_i2c.c", "weight_calibration.c", "weight_command.c")),
            str(ROOT / "test/mini_scales_hal/check_weight_i2c.c"),
            "-lm", "-o", str(cls.executable),
        ], check=True)

    def check_group(self, group):
        result = subprocess.run([str(self.executable), group], check=True,
                                capture_output=True, text=True)
        self.assertIn("checks passed", result.stdout)

    def test_signed_raw_adc_failures_and_recovery(self):
        self.check_group("driver")

    def test_tare_and_transactional_calibration(self):
        self.check_group("calibration")

    def test_command_and_reference_mass_validation(self):
        self.check_group("commands")

    def test_preset_gain_requires_fresh_zero_after_restart(self):
        self.check_group("preset")


if __name__ == "__main__":
    unittest.main()
