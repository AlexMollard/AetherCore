import importlib.util
from pathlib import Path
import unittest
import struct
import tempfile
import zlib
import time
import sys
import tomllib

spec = importlib.util.spec_from_file_location('parity', Path(__file__).with_name('lighting-parity-checks.py'))
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)


class LightingParityTests(unittest.TestCase):
    def test_shadow_center_samples_receiver_outside_caster_silhouette(self):
        image = bytearray([150, 150, 150, 255] * 64 * 64)
        for y in range(28, 37):
            for x in range(28, 37):
                image[(y*64+x)*4:(y*64+x)*4+3] = bytes([0, 0, 0])
        result = parity.feature_checks('BrowserShadowPointOff', image, 64, 64)
        self.assertEqual(result['probes']['center'], [150, 150, 150])
        for kind in ('Sun', 'Point', 'Spot', 'Mask'):
            with self.subTest(kind=kind):
                source = parity.ROOT/'resources/web/reference/scenes'/('BrowserShadow'+kind+'Off.scene.toml')
                scene = tomllib.loads(source.read_text(encoding='utf-8'))
                camera = next(e['camera'] for e in scene['entities'] if 'camera' in e)
                caster = next(e for e in scene['entities'] if e['name'] == 'Shadow caster')
                world_y = camera['orthographic_height']*(.5-parity.SHADOW_RECEIVER_PROBE[1]/64)
                patch_radius = camera['orthographic_height']*2/64
                caster_min_y = caster['position'][1]-caster['scale'][1]/2
                self.assertLess(world_y+patch_radius, caster_min_y)

    def test_native_user_settings_guard_casefolds_table_and_key(self):
        with tempfile.TemporaryDirectory() as directory:
            user = Path(directory)/'UserSettings.toml'
            user.write_text('[GRAPHICS]\nReNdErScAlE = 0.75\n')
            result = parity.native_render_scale_guard(user)
            self.assertFalse(result['qualified'])
            self.assertEqual(result['effectiveValue'], .75)
            self.assertEqual(result['file'], str(user))

    def test_missing_native_user_settings_keeps_project_scale_one(self):
        with tempfile.TemporaryDirectory() as directory:
            user = Path(directory)/'UserSettings.toml'
            result = parity.native_render_scale_guard(user)
            self.assertTrue(result['qualified'])
            self.assertEqual(result['effectiveValue'], 1)
            self.assertFalse(result['fileExists'])

    def test_failed_native_bake_is_rejected_and_owned_process_is_closed(self):
        parent = parity.ROOT/'.temp/lighting-parity'
        parent.mkdir(parents=True, exist_ok=True)
        workers = []
        class FailedProcess:
            def __init__(self, command, cwd, stream):
                self.closed = False
                workers.append(self)
            def poll(self):
                return 1
            def close(self):
                self.closed = True
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            project = Path(directory)/'project'
            project.mkdir()
            (project/'original.gltf').write_text('{"asset":{"version":"2.0"}}')
            with self.assertRaisesRegex(RuntimeError, 'Native canonical model bake failed'):
                parity.bake_native(project, Path(directory), time.monotonic()+10,
                                   worker_factory=FailedProcess, packer=Path(sys.executable))
            self.assertTrue(workers[0].closed)

    def test_native_rgb_png_sub_filter_and_checksum(self):
        def chunk(kind, data):
            return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
        data = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 1, 8, 2, 0, 0, 0))
        data += chunk(b'IDAT', zlib.compress(bytes([1, 51, 102, 153, 10, 10, 10])))+chunk(b'IEND', b'')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'native.png'
            path.write_bytes(data)
            self.assertEqual(parity.png(path), (2, 1, bytes([51, 102, 153, 255, 61, 112, 163, 255])))
            path.write_bytes(data[:-2]+b'xx')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                parity.png(path)

    def test_identical_black_images_do_not_qualify_lighting(self):
        image = bytes([0, 0, 0, 255] * 64 * 64)
        self.assertEqual(parity.compare(image, image, 64, 64)['meanAbsoluteError'], 0)
        self.assertFalse(parity.feature_checks('BrowserSun', image, 64, 64)['passed'])

    def test_swapped_background_channels_fail_parity(self):
        a = bytes([51, 102, 153, 255] * 64 * 64)
        b = bytes([153, 102, 51, 255] * 64 * 64)
        self.assertFalse(parity.compare(a, b, 64, 64)['passed'])
        self.assertTrue(parity.feature_checks('BrowserSolid', a, 64, 64)['passed'])

    def test_shadow_requires_changed_receiver_and_preserves_cutout_orientation(self):
        off = bytes([150, 150, 150, 255] * 64 * 64)
        self.assertFalse(parity.shadow_features('Sun', off, off, 64, 64)['passed'])
        on = bytearray(off)
        for y in range(20, 32):
            for x in range(4, 29):
                on[(y*64+x)*4:(y*64+x)*4+3] = bytes([100, 100, 100])
        inverted = bytearray(off)
        for y in range(32, 44):
            for x in range(4, 29):
                inverted[(y*64+x)*4:(y*64+x)*4+3] = bytes([100, 100, 100])
        for kind in ('Mask', 'Point', 'Spot'):
            with self.subTest(kind=kind):
                self.assertTrue(parity.shadow_features(kind, on, off, 64, 64)['passed'])
                self.assertFalse(parity.shadow_features(kind, inverted, off, 64, 64)['passed'])


if __name__ == '__main__':
    unittest.main()
