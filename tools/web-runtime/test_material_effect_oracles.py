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
        pair = next(p for p in oracle.PAIRS if p['name'] == 'water shading')
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

    def test_backface_auxiliary_must_preserve_the_receiver(self):
        pair = next(p for p in oracle.PAIRS if p['name'] == 'custom graph backface preserves receiver')
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

    def test_data_mip_alpha_coupling_is_rejected(self):
        pair = next(p for p in oracle.PAIRS if p['name'] == 'data mip alpha independence')
        alpha, opaque = self.image(), self.image()
        self.assertTrue(oracle.pair_features(pair, alpha, opaque, 64, 64)['passed'])
        self.change_region(alpha, pair['region'], 110)
        result = oracle.pair_features(pair, alpha, opaque, 64, 64)
        self.assertFalse(result['passed'])
        self.assertFalse(result['checks']['receiverPreserved'])
        with self.assertRaises(ValueError):
            oracle.validate_selection(['BrowserDataMipAlpha'], oracle.GRAPHICS)

    def test_specular_filter_noop_is_rejected(self):
        on, off = self.image(), self.image()
        region = [.3, .3, .7, .7]
        self.assertFalse(oracle.specular_filter_features(on, off, 64, 64, region)['passed'])
        self.change_region(on, region, 110)
        self.assertTrue(oracle.specular_filter_features(on, off, 64, 64, region)['passed'])

    def test_mirrored_tangent_must_reverse_light_response(self):
        mirror, control = self.image(), self.image()
        region = [.3, .3, .7, .7]
        self.assertFalse(oracle.mirrored_normal_features(mirror, control, 64, 64, region)['passed'])
        self.change_region(control, region, 120)
        self.assertTrue(oracle.mirrored_normal_features(mirror, control, 64, 64, region)['passed'])
        self.assertFalse(oracle.mirrored_normal_features(control, mirror, 64, 64, region)['passed'])

    def test_cutout_shadow_rejects_solid_or_missing_shadow(self):
        lit, cutout, solid = self.image(), self.image(), self.image()
        hole, opaque = [.3, .3, .4, .4], [.5, .5, .6, .6]
        self.change_region(solid, hole, 70)
        self.change_region(cutout, opaque, 70)
        self.assertTrue(oracle.cutout_shadow_features(cutout, lit, solid, 64, 64, hole, opaque)['passed'])
        self.assertFalse(oracle.cutout_shadow_features(solid, lit, solid, 64, 64, hole, opaque)['passed'])
        self.assertFalse(oracle.cutout_shadow_features(lit, lit, solid, 64, 64, hole, opaque)['passed'])

    def test_cross_product_requires_complete_controls(self):
        for selection in (['BrowserCrossMaskOn'], ['BrowserCrossSkinOn'], ['BrowserCrossMirror']):
            with self.assertRaises(ValueError):
                oracle.validate_selection(selection, oracle.GRAPHICS)


if __name__ == '__main__':
    unittest.main()
