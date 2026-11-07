"""Python-only checks; these never build or upload firmware."""
from pathlib import Path
import tempfile
import unittest

import numpy as np

from tools.calibrate_adc_run import fit, install_profile, online_ema, resolve_aligned


class RawAdcCalibrationTests(unittest.TestCase):
    def test_directory_resolves_unique_run_and_requires_its_student_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            aligned = root / 'run_1_aligned.csv'
            aligned.touch()
            with self.assertRaisesRegex(ValueError, '缺少同组'):
                resolve_aligned(root)
            (root / 'run_1_student.csv').touch()
            self.assertEqual(resolve_aligned(root), aligned.resolve())
            self.assertEqual(resolve_aligned(aligned), aligned.resolve())

    def test_empty_or_multiple_runs_never_silently_choose_a_calibration(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with self.assertRaisesRegex(ValueError, '没有找到'):
                resolve_aligned(root)
            for name in ('run_1', 'run_2'):
                (root / f'{name}_aligned.csv').touch()
                (root / f'{name}_student.csv').touch()
            with self.assertRaisesRegex(ValueError, '有多组数据'):
                resolve_aligned(root)
            self.assertEqual(resolve_aligned(root, 'run_2'), (root / 'run_2_aligned.csv').resolve())

    def test_held_step_is_filtered_at_each_5ms_row_and_is_causal(self):
        t = np.arange(201) * .005
        raw = np.r_[0., np.full(200, 25000.)]
        actual = online_ema(t, raw, 0)
        expected = 25000. * (1 - np.exp(-t / .2))
        np.testing.assert_allclose(actual, expected, atol=1e-9, rtol=0)
        raw[100:] = -100000.
        np.testing.assert_array_equal(online_ema(t, raw, 0)[:100], actual[:100])

    def test_timestamp_ties_do_not_advance_time_and_gap_reseeds(self):
        t = np.array([0., .005, .005, .010, .260])
        raw = np.array([10., 20., 30., 40., 100.])
        result = online_ema(t, raw, 0)
        self.assertEqual(result[1], result[2])
        self.assertEqual(result[-1], 100.)
        self.assertTrue(np.isnan(online_ema(t, raw, 2)[:2]).all())

    def test_fixed_twelve_knots_with_origin_keep_nonpositive_delta_in_score(self):
        x = np.r_[-100., 0., np.linspace(1000., 400000., 100)]
        truth = np.linspace(2., 10., len(x))
        knots, values, predicted = fit(x, truth)
        self.assertEqual(len(knots), 13)
        self.assertEqual((knots[0], values[0]), (0., 0.))
        self.assertTrue(np.all(np.diff(knots) > 0))
        self.assertTrue(np.all(np.diff(values) >= 0))
        self.assertEqual(len(predicted), len(x))
        np.testing.assert_array_equal(predicted[:2], [0., 0.])
        self.assertTrue(np.all((predicted[:2] - truth[:2]) ** 2 >= 4.))

    def test_profile_write_preserves_backup_and_unrelated_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            profile = root / 'profile.h'
            profile.write_bytes(b'old parameters\n')
            raw_source = root / 'raw_adc.c'
            raw_source.write_bytes(b'unchanged\n')
            output = root / 'result'
            output.mkdir()
            install_profile(profile, 'new parameters\n', b'old parameters\n', output)
            self.assertEqual(profile.read_bytes(), b'new parameters\n')
            self.assertEqual((output / 'previous_profile.h').read_bytes(), b'old parameters\n')
            self.assertEqual(raw_source.read_bytes(), b'unchanged\n')
            with self.assertRaises(ValueError):
                install_profile(profile, 'wrong\n', b'old parameters\n', output)
            self.assertEqual(profile.read_bytes(), b'new parameters\n')


if __name__ == '__main__':
    unittest.main()
