"""Independent examples for whole-boot-block cost decisions."""

import copy
import unittest
from unittest.mock import patch

import cost_match_report as REPORT


def constant_blocks(before=100, middle=100, after=100):
    return [dict(before_ns=before, middle_ns=middle, after_ns=after)
            for _ in range(12)]


class CostMatchReportTest(unittest.TestCase):
    def analyse(self, blocks, mode="comparison", **kwargs):
        return REPORT.analyse_blocks(blocks, mode=mode, **kwargs)

    def test_constant_same_kernel_calibrates_with_zero_error(self):
        result = self.analyse(constant_blocks(), "calibration")
        self.assertEqual(result["classification"], "pass")
        self.assertTrue(result["calibration_pass"])
        self.assertEqual(result["medians_ns"],
                         {"before": 100, "middle": 100, "after": 100})
        self.assertEqual(result["estimate_percent"], 0)
        self.assertEqual(result["drift_percent"], 0)
        for field in ("lower95_percent", "upper95_percent",
                      "lower_two95_percent", "upper_two95_percent"):
            self.assertEqual(result[field], 0)
        self.assertEqual(result["bootstrap_draws"], 10000)
        self.assertEqual(result["block_count"], 12)

    def test_comparison_needs_successful_calibration_even_for_a_clear_gain(self):
        for ratio in (90, 110):
            with self.subTest(middle=ratio):
                result = self.analyse(constant_blocks(middle=ratio))
                self.assertEqual(result["classification"], "unresolved")
                self.assertFalse(result["calibration_pass"])
                self.assertIn("calibration-missing", result["reasons"])

    def test_three_percent_boundary_passes_but_above_is_regression(self):
        for middle, expected, percent in ((103, "pass", 3),
                                          (104, "regression", 4),
                                          (97, "pass", -3)):
            with self.subTest(middle=middle):
                result = self.analyse(constant_blocks(middle=middle),
                                      calibration_pass=True)
                self.assertEqual(result["classification"], expected)
                self.assertEqual(result["estimate_percent"], percent)
                self.assertEqual(result["lower95_percent"], percent)
                self.assertEqual(result["upper95_percent"], percent)

    def test_calibration_accepts_exact_tolerance_and_rejects_false_gain(self):
        for middle, expected in ((99, "pass"), (101, "pass"),
                                  (98, "unresolved"), (102, "unresolved")):
            with self.subTest(middle=middle):
                result = self.analyse(constant_blocks(middle=middle), "calibration")
                self.assertEqual(result["classification"], expected)
                self.assertEqual(result["calibration_pass"], expected == "pass")
                if expected == "unresolved":
                    self.assertIn("calibration-interval-outside-tolerance",
                                  result["reasons"])

    def test_two_percent_drift_boundary_is_inclusive(self):
        for after, expected, drift in ((102, "pass", 2),
                                       (98, "pass", -2),
                                       (103, "unresolved", 3),
                                       (97, "unresolved", -3)):
            with self.subTest(after=after):
                # middle equals the exact mean of both baseline medians.
                result = self.analyse(constant_blocks(before=200,
                                                      middle=100 + after,
                                                      after=2 * after),
                                      calibration_pass=True)
                self.assertEqual(result["estimate_percent"], 0)
                self.assertEqual(result["drift_percent"], drift)
                self.assertEqual(result["classification"], expected)
                if expected == "unresolved":
                    self.assertIn("drift-exceeds-limit", result["reasons"])

    def test_calibration_rejects_drift_even_with_zero_false_difference(self):
        result = self.analyse(constant_blocks(before=200, middle=203, after=206),
                              "calibration")
        self.assertEqual(result["estimate_percent"], 0)
        self.assertEqual(result["drift_percent"], 3)
        self.assertEqual(result["classification"], "unresolved")
        self.assertFalse(result["calibration_pass"])
        self.assertEqual(result["reasons"], ["drift-exceeds-limit"])

    def test_even_sample_medians_keep_half_nanoseconds(self):
        blocks = constant_blocks(before=1, middle=2, after=1)[:6]
        blocks += constant_blocks(before=2, middle=4, after=2)[:6]
        result = self.analyse(blocks, calibration_pass=True)
        self.assertEqual(result["medians_ns"],
                         {"before": 1.5, "middle": 3, "after": 1.5})
        self.assertEqual(result["estimate_percent"], 100)
        self.assertEqual(result["classification"], "regression")

    def test_medians_are_combined_before_division_not_block_ratios_averaged(self):
        blocks = constant_blocks(middle=102)[:10]
        blocks += [dict(before_ns=1000, middle_ns=1, after_ns=1000)
                   for _ in range(2)]
        result = self.analyse(blocks, calibration_pass=True)
        self.assertEqual(result["medians_ns"],
                         {"before": 100, "middle": 102, "after": 100})
        self.assertEqual(result["estimate_percent"], 2)

    def test_whole_triplet_sampling_preserves_paired_scale(self):
        # Every complete triplet has the same +2% ratio. Boot duration varies
        # sharply, so independently resampling columns would invent uncertainty.
        scales = (1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233)
        blocks = [dict(before_ns=100 * n, middle_ns=102 * n, after_ns=100 * n)
                  for n in scales]
        result = self.analyse(blocks, calibration_pass=True)
        self.assertEqual(result["classification"], "pass")
        for field in ("estimate_percent", "lower95_percent", "upper95_percent",
                      "lower_two95_percent", "upper_two95_percent"):
            self.assertEqual(result[field], 2)

    def test_asymmetric_uncertainty_is_not_replaced_with_symmetric_error(self):
        blocks = constant_blocks(middle=102)[:7]
        blocks += constant_blocks(middle=140)[:5]
        result = self.analyse(blocks, calibration_pass=True)
        self.assertEqual(result["estimate_percent"], 2)
        self.assertEqual(result["lower95_percent"], 2)
        self.assertEqual(result["upper95_percent"], 40)
        self.assertEqual(result["classification"], "unresolved")
        self.assertIn("interval-crosses-threshold", result["reasons"])

    def test_nearest_rank_percentiles_use_observed_values(self):
        # Control only index generation. Each bootstrap draw selects one full
        # triplet twelve times; the real estimator and classification run.
        # Ordered draw counts place ranks 250/500/9500/9750 exactly at changes.
        indices = ([0] * 249 + [1] * 250 + [2] * 9000 +
                   [3] * 250 + [4] * 251)

        class ControlledRandom:
            def __init__(self, seed):
                self.call = 0

            def randrange(self, stop):
                if stop != 12:
                    raise AssertionError("resampling changed the block count")
                index = indices[self.call // 12]
                self.call += 1
                return index

        blocks = [dict(before_ns=100, middle_ns=(i + 1) * 10, after_ns=100)
                  for i in range(12)]
        with patch.object(REPORT.random, "Random", ControlledRandom):
            result = self.analyse(blocks, calibration_pass=True)
        self.assertEqual(result["lower_two95_percent"], -80)
        self.assertEqual(result["lower95_percent"], -70)
        self.assertEqual(result["upper95_percent"], -60)
        self.assertEqual(result["upper_two95_percent"], -50)

    def test_fixed_seed_is_deterministic_and_input_is_unchanged(self):
        blocks = [dict(before_ns=100 + i, middle_ns=95 + 3 * i,
                       after_ns=101 + i) for i in range(12)]
        frozen = copy.deepcopy(blocks)
        first = self.analyse(blocks, calibration_pass=True, seed=123)
        second = self.analyse(blocks, calibration_pass=True, seed=123)
        self.assertEqual(first, second)
        self.assertEqual(blocks, frozen)

    def test_missing_extra_and_nonpositive_measurements_are_invalid(self):
        for change in ({"before_ns": 0}, {"middle_ns": -1},
                       {"after_ns": True}, {"before_ns": 1.0},
                       {"middle_ns": float("nan")},
                       {"after_ns": float("inf")},
                       {"before_ns": 10 ** 1000},
                       {"before_ns": "100"}, {"extra": 1}):
            with self.subTest(change=change):
                blocks = constant_blocks()
                blocks[0].update(change)
                with self.assertRaises(ValueError):
                    self.analyse(blocks, "calibration")
        blocks = constant_blocks()
        del blocks[0]["after_ns"]
        with self.assertRaises(ValueError):
            self.analyse(blocks, "calibration")

    def test_incomplete_extra_and_invalid_block_sequences_are_invalid(self):
        for blocks in (constant_blocks()[:11], constant_blocks() + constant_blocks()[:1],
                       None, "invalid", {}, [None] * 12,
                       (block for block in constant_blocks())):
            with self.subTest(blocks=type(blocks).__name__):
                with self.assertRaises(ValueError):
                    self.analyse(blocks, "calibration")

    def test_invalid_mode_seed_or_calibration_flag_is_rejected(self):
        for mode in (None, "pilot", "", True):
            with self.subTest(mode=mode):
                with self.assertRaises(ValueError):
                    self.analyse(constant_blocks(), mode)
        for seed in (None, "123", 1.0, True, -1):
            with self.subTest(seed=seed):
                with self.assertRaises(ValueError):
                    self.analyse(constant_blocks(), seed=seed)
        for flag in (None, "passed", 1, 0):
            with self.subTest(calibration_pass=flag):
                with self.assertRaises(ValueError):
                    self.analyse(constant_blocks(), calibration_pass=flag)


if __name__ == "__main__":
    unittest.main()
