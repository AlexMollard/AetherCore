import importlib.util
from pathlib import Path
from types import SimpleNamespace
import tempfile
import time
import tomllib
import unittest

spec=importlib.util.spec_from_file_location('post_parity',Path(__file__).with_name('post-parity-checks.py'))
post=importlib.util.module_from_spec(spec)
spec.loader.exec_module(post)


class PostParityTests(unittest.TestCase):
    def test_unchanged_images_cannot_qualify_any_spatial_feature(self):
        image=bytes([120,120,120,255]*64*64)
        for feature in post.SPATIAL:
            with self.subTest(feature=feature):
                self.assertFalse(post.effect_checks(feature,image,image,64,64)['passed'])

    def test_vignette_requires_corner_darkening_not_brightness_shift(self):
        off=bytes([120,120,120,255]*64*64)
        shifted=bytes([80,80,80,255]*64*64)
        self.assertFalse(post.effect_checks('vignette',shifted,off,64,64)['passed'])
        vignette=bytearray(off)
        for y in range(10):
            for x in range(10):
                vignette[(y*64+x)*4:(y*64+x)*4+3]=bytes([80,80,80])
        self.assertTrue(post.effect_checks('vignette',vignette,off,64,64)['passed'])

    def test_saturation_rejects_unchanged_color_or_dark_output(self):
        color=bytes([180,80,30,255]*64*64)
        neutral=bytes([100,100,100,255]*64*64)
        self.assertTrue(post.effect_checks('saturation',neutral,color,64,64)['passed'])
        self.assertFalse(post.effect_checks('saturation',color,color,64,64)['passed'])
        self.assertFalse(post.effect_checks('saturation',neutral,bytes([0,0,0,255]*64*64),64,64)['passed'])

    def test_temporal_effect_cannot_pass_as_ordinary_image_pair(self):
        image=bytes([120,120,120,255]*64*64)
        for feature in post.TEMPORAL:
            with self.subTest(feature=feature),self.assertRaisesRegex(ValueError,'accepted-frame'):
                post.effect_checks(feature,image,image,64,64)

    def test_analytic_exposure_has_native_three_slot_six_frame_cadence(self):
        self.assertEqual(post.expected_exposure(0),1)
        self.assertEqual(post.expected_exposure(2),1)
        self.assertLess(post.expected_exposure(3),1)
        self.assertEqual(post.expected_exposure(8),post.expected_exposure(3))
        self.assertLess(post.expected_exposure(9),post.expected_exposure(8))
        self.assertEqual(post.expected_exposure(14),post.expected_exposure(9))

    def test_exposure_requires_actual_gpu_value_and_frame_alignment(self):
        on=bytes([200,200,200,255]*64*64)
        off=bytes([210,210,210,255]*64*64)
        native=dict(frameIndex=9,elapsedTime=10/60,exposureUsed=post.expected_exposure(9))
        web=dict(frameIndex=12,temporalFrameIndex=9,simulationTick=10,exposureUsed=post.expected_exposure(9))
        case=dict(nativeFrameEvidence=native,webFrameEvidence=web)
        control=dict(nativeFrameEvidence=dict(native,exposureUsed=1),webFrameEvidence=dict(web,exposureUsed=1))
        for backend in ('native','web'):
            self.assertTrue(post.temporal_checks('auto-exposure',on,off,64,64,case,control,backend)['passed'])
        bad=dict(case,webFrameEvidence=dict(web,exposureUsed=.5))
        self.assertFalse(post.temporal_checks('auto-exposure',on,off,64,64,bad,control,'web')['passed'])
        wrong_frame=dict(case,webFrameEvidence=dict(web,temporalFrameIndex=8))
        self.assertFalse(post.temporal_checks('auto-exposure',on,off,64,64,wrong_frame,control,'native')['passed'])
        self.assertFalse(post.temporal_checks('auto-exposure',on,off,64,64,{},control,'native')['passed'])

    def test_fixture_maps_are_native_keys_and_motion_uses_existing_rust_provider(self):
        for scene,overrides in post.FEATURES.values():
            self.assertEqual(post.parity.graphics_settings(overrides)|overrides,post.parity.graphics_settings(overrides))
            path=post.ROOT/'resources/web/reference/scenes'/(scene+'.scene.toml')
            fixture=tomllib.loads(path.read_text())
            self.assertEqual(fixture['scene']['kind'],'3d')
        motion=tomllib.loads((post.ROOT/'resources/web/reference/scenes/BrowserPostMotion.scene.toml').read_text())
        camera=next(entity for entity in motion['entities'] if 'camera' in entity)
        self.assertEqual(camera['scripts'][0]['type'],'ModelProbe')

    def test_owned_child_is_closed_even_when_no_report_exists(self):
        workers=[]
        class Worker:
            def __init__(self,*args): self.closed=False;workers.append(self)
            def poll(self): return 1
            def close(self): self.closed=True
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError,'no maintained report'):
                post.run_capture('BrowserPostPattern',{},'failure',SimpleNamespace(build=Path(directory),playwright=None),Path(directory),time.monotonic()+40,Worker)
            self.assertTrue(workers[0].closed)


if __name__=='__main__':
    unittest.main()
