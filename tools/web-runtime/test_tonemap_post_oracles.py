import importlib.util
import math
from pathlib import Path
import re
import tomllib
import unittest

spec=importlib.util.spec_from_file_location('tonemap_oracles',Path(__file__).with_name('tonemap_post_oracles.py'))
oracle=importlib.util.module_from_spec(spec)
spec.loader.exec_module(oracle)


class TonemapOracleTests(unittest.TestCase):
    def test_analytic_meter_fixtures_exclude_unmetered_pbr_sky_radiance(self):
        scenes=Path(__file__).resolve().parents[2]/'resources/web/reference/scenes'
        for name,count in (('BrowserPostTonemap',8),('BrowserPostExposure',1)):
            with self.subTest(scene=name):
                scene=tomllib.loads((scenes/(name+'.scene.toml')).read_text())
                materials=[entity['material'] for entity in scene['entities'] if 'material' in entity]
                self.assertEqual(len(materials),count)
                for material in materials:
                    self.assertTrue(material['baked_lighting'])
                    self.assertEqual(material['base_color'],[0,0,0,1])
                    self.assertFalse(material['receive_shadows'])

    def test_all_public_native_modes_have_one_profile(self):
        source=(Path(__file__).resolve().parents[2]/'src/engine/passes/TonemapDefs.hpp').read_text()
        names=re.findall(r'TonemapDef\{TonemapMode::\w+, "([^"]+)"\}',source)
        self.assertEqual(set(names),set(oracle.PROFILES.values()))
        self.assertEqual(len(oracle.PROFILES),11)

    def test_all_curves_are_finite_bounded_and_resolve_nonfallback_response(self):
        for mode in oracle.MODES:
            with self.subTest(mode=mode):
                values=[oracle.display_rgb(mode,value) for value in oracle.RADIANCES]
                self.assertTrue(all(math.isfinite(c) and 0<=c<=255 for rgb in values for c in rgb))
                self.assertGreater(values[-1][0]-values[4][0],20)
                if mode!='ACES Filmic':
                    self.assertGreater(max(abs(a-b) for x in oracle.RADIANCES for a,b in zip(oracle.display_rgb(mode,x),oracle.display_rgb('ACES Filmic',x))),4)

    def test_encoded_curves_do_not_receive_gamma_twice(self):
        self.assertAlmostEqual(oracle.display_rgb('Hejl Richard',.004)[0],0)
        self.assertGreater(oracle.display_rgb('Filmic Dice',.004)[0],7)
        self.assertAlmostEqual(oracle.display_rgb('Reinhard',1)[0],255*.5**(1/2.2))
        self.assertAlmostEqual(oracle.display_rgb('Exponential',1)[0],255*(1-math.exp(-1))**(1/2.2))

    def test_identity_operators_intentionally_match_display_clipping(self):
        for value in oracle.RADIANCES:
            self.assertEqual(oracle.display_rgb('Linear',value),oracle.display_rgb('Vanilla',value))

    def test_all_eight_band_oracles_reject_aces_fallback(self):
        def image(mode):
            row=b''.join(bytes([*map(round,oracle.display_rgb(mode,value)),255])*8 for value in oracle.RADIANCES)
            return row*64
        patch=lambda image,width,height,x,y,world:list(image[(int(y)*width+int(x))*4:(int(y)*width+int(x))*4+3])
        control=image('ACES Filmic')
        for feature,mode in oracle.PROFILES.items():
            with self.subTest(mode=mode):
                self.assertTrue(oracle.checks(feature,image(mode),control,64,64,patch)['passed'])
                if mode!='ACES Filmic':
                    self.assertFalse(oracle.checks(feature,control,control,64,64,patch)['passed'])


if __name__=='__main__':unittest.main()
