import unittest

from ui_parity_oracles import scene_features, shader_features


class UiParityOracleTests(unittest.TestCase):
    def test_blank_agreement_fails(self):
        blank = bytes([50, 100, 150, 255]) * (512 * 512)
        result = scene_features(blank, blank, 512, 512)
        self.assertFalse(result['passed'])
        self.assertFalse(result['checks']['buttonBodyVisible'])
        self.assertFalse(result['checks']['descendantDrawnOverParents'])
    def test_missing_shaders_fail_pair(self):
        blank = bytes([50, 100, 150, 255]) * (512 * 512)
        result = shader_features(blank, blank, 512, 512)
        self.assertFalse(result['passed'])
        self.assertFalse(result['checks']['backgroundEffectVisible'])
        self.assertFalse(result['checks']['overlayEffectVisible'])
        self.assertFalse(result['checks']['elementMaterialVisible'])


    def test_rejects_wrong_extent(self):
        with self.assertRaises(ValueError):
            scene_features(bytes(64 * 64 * 4), bytes(64 * 64 * 4), 64, 64)


if __name__ == '__main__':
    unittest.main()
