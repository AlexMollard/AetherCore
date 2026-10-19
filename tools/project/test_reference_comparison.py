import unittest

from compare_reference import compare_observations


class ObservableComparisonTests(unittest.TestCase):
    def observation(self, actual, tolerance=0):
        return {"assertions": [dict(entity="Player", type="$engine", property="position", actual=actual, tolerance=tolerance, passed=True)]}

    def test_physics_tolerances_do_not_require_identical_float_bits(self):
        self.assertEqual(compare_observations(self.observation([1, 0, 0], .05), self.observation([1.01, 0, 0], .05)), [])

    def test_missing_or_failed_observations_cannot_compare_as_success(self):
        self.assertTrue(compare_observations(self.observation(20), {"assertions": []}))
        other = self.observation(20)
        other["assertions"][0]["passed"] = False
        self.assertTrue(compare_observations(self.observation(20), other))

    def test_equal_integer_outcomes_and_unicode_are_exact(self):
        self.assertTrue(compare_observations(self.observation(20), self.observation(21)))
        self.assertTrue(compare_observations(self.observation("caf\u00e9\0agent"), self.observation("caf\u00e9")))

    def test_readiness_uses_first_tick_receipt_and_rejects_clock_jumps(self):
        from compare_reference import readiness_ms
        self.assertEqual(readiness_ms({"firstTickUnixNs": 1_500_000_000}, 1_000_000_000, 1000), 500)
        with self.assertRaises(ValueError):
            readiness_ms({"firstTickUnixNs": 900_000_000}, 1_000_000_000, 1000)
        with self.assertRaises(ValueError):
            readiness_ms({"firstTickUnixNs": 3_000_000_000}, 1_000_000_000, 1000)


if __name__ == "__main__":
    unittest.main()
