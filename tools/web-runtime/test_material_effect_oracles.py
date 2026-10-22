import unittest

import material_effect_oracles as oracle


class MaterialEffectOracleTests(unittest.TestCase):
    def image(self, value=100):
        return bytearray([value, value, value, 255] * (64 * 64))

    def change_region(self, image, region, value):
        x0, y0, x1, y1 = [int(v * 64) for v in region]
        for y in range(y0, y1):
            for x in range(x0, x1):
                image[(y * 64 + x) * 4:(y * 64 + x) * 4 + 3] = bytes([value] * 3)

    def test_noop_pair_fails_even_when_both_backends_could_match(self):
        pair = oracle.PAIRS[0]
        result = oracle.pair_features(pair, self.image(), self.image(), 64, 64)
        self.assertFalse(result['passed'])
        self.assertFalse(result['checks']['authoredReceiverChanges'])

    def test_direction_and_independent_corner_are_enforced(self):
        pair = next(p for p in oracle.PAIRS if p.get('direction') == 'darker')
        off, on = self.image(), self.image()
        self.change_region(on, pair['region'], 70)
        self.assertTrue(oracle.pair_features(pair, on, off, 64, 64)['passed'])
        self.change_region(on, pair['region'], 130)
        self.assertFalse(oracle.pair_features(pair, on, off, 64, 64)['passed'])
        self.change_region(on, pair['region'], 70)
        self.change_region(on, [.02, .02, .08, .08], 130)
        self.assertFalse(oracle.pair_features(pair, on, off, 64, 64)['passed'])

    def test_selection_requires_controls_and_effect_settings(self):
        with self.assertRaises(ValueError):
            oracle.validate_selection(['BrowserWater'], oracle.GRAPHICS)
        with self.assertRaises(ValueError):
            oracle.validate_selection(['BrowserContact', 'BrowserContactOff'], {})
        with self.assertRaises(ValueError):
            oracle.validate_selection(['BrowserFoliage', 'BrowserFoliageOff'], {})
        oracle.validate_selection(oracle.SCENES, oracle.GRAPHICS)
        self.assertEqual(len(oracle.SCENES), 19)

    def test_backface_auxiliary_must_preserve_the_receiver(self):
        pair = next(p for p in oracle.PAIRS if p.get('direction') == 'unchanged')
        on, off = self.image(), self.image()
        self.assertTrue(oracle.pair_features(pair, on, off, 64, 64)['passed'])
        self.change_region(on, pair['region'], 0)
        self.assertFalse(oracle.pair_features(pair, on, off, 64, 64)['passed'])
        with self.assertRaises(ValueError):
            oracle.validate_selection(['BrowserGraphBackface', 'BrowserGraphBackfaceOff'], oracle.GRAPHICS)

    def test_display_only_particles_cannot_pass_as_an_empty_frame(self):
        image = self.image(0)
        sample = lambda x, y, world=True: list(image[(y * 64 + x) * 4:(y * 64 + x) * 4 + 3])
        self.assertFalse(oracle.scene_features('BrowserParticlesDisplayOnly', image, 64, 64, sample)['passed'])
        self.change_region(image, [.45, .45, .55, .55], 180)
        self.assertTrue(oracle.scene_features('BrowserParticlesDisplayOnly', image, 64, 64, sample)['passed'])


if __name__ == '__main__':
    unittest.main()
