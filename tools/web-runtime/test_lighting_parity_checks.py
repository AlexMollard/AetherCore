import importlib.util
from pathlib import Path
import unittest
import struct
import tempfile
import zlib

spec = importlib.util.spec_from_file_location('parity', Path(__file__).with_name('lighting-parity-checks.py'))
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)


class LightingParityTests(unittest.TestCase):
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
        self.assertTrue(parity.shadow_features('Mask', on, off, 64, 64)['passed'])
        inverted = bytearray(off)
        for y in range(32, 44):
            for x in range(4, 29):
                inverted[(y*64+x)*4:(y*64+x)*4+3] = bytes([100, 100, 100])
        self.assertFalse(parity.shadow_features('Mask', inverted, off, 64, 64)['passed'])


if __name__ == '__main__':
    unittest.main()
