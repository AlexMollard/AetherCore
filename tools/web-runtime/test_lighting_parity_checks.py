import importlib.util
from pathlib import Path
import unittest
import struct
import tempfile
import zlib
import time
import sys
import tomllib
from unittest.mock import patch
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location('parity', Path(__file__).with_name('lighting-parity-checks.py'))
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)


class LightingParityTests(unittest.TestCase):
    def test_post_corner_requires_actual_colored_cubes_and_receiver(self):
        wall=bytes([120,128,142,255]*64*64)
        self.assertFalse(parity.feature_checks('BrowserPostCorner',wall,64,64)['passed'])
        image=bytearray(wall)
        for cx,cy,color in ((24,36,[120,45,29]),(39,34,[0,64,166]),(32,52,[16,54,98])):
            for y in range(cy-3,cy+4):
                for x in range(cx-3,cx+4):
                    image[(y*64+x)*4:(y*64+x)*4+3]=bytes(color)
        for scene in ('BrowserPostCorner','BrowserPostVolumetric'):
            with self.subTest(scene=scene):
                self.assertTrue(parity.feature_checks(scene,image,64,64)['passed'])
        for y in range(31,38):
            for x in range(36,43):
                image[(y*64+x)*4:(y*64+x)*4+3]=bytes([120,128,142])
        self.assertFalse(parity.feature_checks('BrowserPostCorner',image,64,64)['passed'],'A missing blue reflection source cannot qualify')

    def test_frame_receipts_require_actual_identity_fixed_clock_and_gpu_source_extent(self):
        native=dict(frameIndex=9,elapsedTime=10/60,exposureUsed=1,width=513,height=511,renderWidth=256,renderHeight=255)
        web=dict(frameIndex=12,temporalFrameIndex=9,simulationTick=10,exposureUsed=1,width=513,height=511,renderWidth=256,renderHeight=255)
        self.assertTrue(parity.validate_frame_evidence(native,web,513,511,.5,True)['passed'])
        for n,w in ((None,web),(native,None),(dict(native,elapsedTime=.12),web),
                    (dict(native,exposureUsed=float('nan')),web),(native,dict(web,frameIndex=True)),
                    (native,dict(web,temporalFrameIndex=8)),(native,dict(web,simulationTick=9)),
                    (dict(native,renderWidth=257),web),(native,dict(web,renderHeight=256))):
            with self.subTest(native=n,web=w),self.assertRaisesRegex(ValueError,'UNQUALIFIED'):
                parity.validate_frame_evidence(n,w,513,511,.5,True)

    def test_graph_oracle_rejects_black_and_default_gray_materials(self):
        for rgb in ((0, 0, 0), (160, 160, 160)):
            image = bytes([*rgb, 255]*64*64)
            self.assertFalse(parity.feature_checks('BrowserGraph', image, 64, 64)['passed'])
        image = bytearray([0, 0, 0, 255]*64*64)
        for y in range(8, 56):
            for x in range(8, 56):
                image[(y*64+x)*4:(y*64+x)*4+3] = bytes([180, 90, 35])
        self.assertTrue(parity.feature_checks('BrowserGraph', image, 64, 64)['passed'])
        for y in range(27, 38):
            for x in range(27, 38):
                image[(y*64+x)*4:(y*64+x)*4+3] = bytes([165, 115, 108])
        self.assertTrue(parity.feature_checks('BrowserGraph', image, 64, 64)['passed'], 'Center specular highlight must not substitute for red body probes')

    def test_native_graph_cook_stages_editor_overlay_before_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root/'.temp/lighting-parity/private'
            material = project/'assets/materials/browser-graph.material.toml'
            material.parent.mkdir(parents=True)
            material.write_text('authored graph')
            seen = []
            def cook(*args):
                seen.append(args)
                return dict(output=str(args[2]/'BrowserGraph.spv'), sha256='fixture')
            shim = SimpleNamespace(resolve_compiler=lambda path: path, cook_native=cook)
            with patch.object(parity, 'ROOT', root), patch.dict(sys.modules, web_materials=shim):
                result = parity.native_graph_cook(project, root/'build/webgpu-browser', time.monotonic()+30, ['BrowserGraph'], compiler=Path('compiler'), slang=Path('slang'), cook_function=cook)
            self.assertEqual(result['status'], 'passed')
            self.assertEqual(seen[0][2], project.resolve()/'Builds/Intermediate/shaders')
            self.assertEqual(result['runtimeUri'], 'shaders://BrowserGraph.spv')
            self.assertEqual(material.read_text(), 'authored graph')
            self.assertEqual(parity.native_graph_cook(project, root, 0, ['BrowserSun'])['status'], 'skipped')

    def test_graphics_override_is_identical_typed_data_and_keeps_defaults(self):
        original = dict(parity.GRAPHICS)
        result = parity.graphics_settings(dict(bloomStrength=.3, gtao=True, vignetteRoundness=.7, tonemap='Linear'))
        self.assertEqual(result['bloomStrength'], .3)
        self.assertTrue(result['gtao'])
        self.assertEqual(result['exposure'], 1)
        self.assertEqual(parity.GRAPHICS, original)
        for invalid in ([], {'unknownEffect':1}, {'fxaa':1}, {'bloomStrength':True}, {'exposure':float('nan')},
                        {'renderScale':.125}, {'tonemap':'mystery'}, {'anisotropy':1.5}):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                parity.graphics_settings(invalid)
        self.assertEqual(parity.graphics_settings({'renderScale':.5})['renderScale'],.5)

    def test_scheduled_changes_land_before_capture_and_end_at_requested_extent(self):
        final, start = (512, 512), (384, 320)
        valid = [dict(tick=4, graphics={'autoExposure': True}), dict(tick=9, resize=[512, 512])]
        self.assertEqual(parity.scheduled_changes(valid, final, start), valid)
        self.assertEqual(parity.scheduled_changes([dict(tick=0, graphics={'gtao': True})], final, final)[0]['tick'], 0)
        for changes, initial in (([dict(tick=10, graphics={'gtao': True})], final),
                                 ([dict(tick=3)], final),
                                 ([dict(tick=3, graphics={})], final),
                                 ([dict(tick=3, graphics={'unknownEffect': 1})], final),
                                 ([dict(tick=3, graphics={'gtao': True}, extra=1)], final),
                                 ([dict(tick=3, resize=[640, 480])], start),
                                 ([dict(tick=3, resize=[512, 512])], final),
                                 ([dict(tick=3, graphics={'gtao': True})], start)):
            with self.subTest(changes=changes, initial=initial), self.assertRaises(ValueError):
                parity.scheduled_changes(changes, final, initial)

    def test_effect_controls_cannot_qualify_identical_images(self):
        image = bytes([150, 120, 90, 255]*64*64)
        for kind in parity.EFFECTS:
            with self.subTest(kind=kind):
                self.assertFalse(parity.effect_features(kind,image,image,64,64)['passed'])
        for scene in parity.EFFECT_SCENES:
            with self.subTest(scene=scene):
                black = bytes([0,0,0,255]*64*64)
                self.assertFalse(parity.feature_checks(scene,black,64,64)['passed'])

    def test_effect_control_fixtures_change_exactly_one_authored_field(self):
        for kind in parity.EFFECTS:
            on = 'Browser'+kind+('On' if kind == 'Dof' else '')
            off = 'Browser'+kind+'Off'
            source = parity.ROOT/'resources/web/reference/scenes'
            a = (source/(on+'.scene.toml')).read_text().splitlines()
            b = (source/(off+'.scene.toml')).read_text().splitlines()
            with self.subTest(kind=kind):
                self.assertEqual(len(a),len(b))
                self.assertEqual(sum(left != right for left,right in zip(a,b)),1)
                tomllib.loads('\n'.join(a))
                tomllib.loads('\n'.join(b))

    def test_non_casting_mesh_stays_visible_and_leaves_receiver_lit(self):
        clear = bytes([150, 150, 150, 255] * 64 * 64)
        self.assertFalse(parity.feature_checks('BrowserShadowNoCast', clear, 64, 64)['passed'], 'Removing the entire caster must not pass')
        image = bytearray(clear)
        for y in range(28, 33):
            for x in range(28, 37):
                image[(y*64+x)*4:(y*64+x)*4+3] = bytes([30, 30, 30])
        self.assertTrue(parity.feature_checks('BrowserShadowNoCast', image, 64, 64)['passed'])
        for y in range(27, 34):
            for x in range(17, 24):
                image[(y*64+x)*4:(y*64+x)*4+3] = bytes([30, 30, 30])
        self.assertFalse(parity.feature_checks('BrowserShadowNoCast', image, 64, 64)['passed'], 'A visible mesh that still casts must not pass')

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

    CUSTOM_PASS_SIZE = 512

    @classmethod
    def custom_pass_pair(cls, *, behind_over_sprite=False, unlit_over=False, lit_emissive_dimmed=False, flip_borders=False):
        # Ideal BrowserCustomPass capture at 512 px (8 px per 64-lattice unit): sprite and Over rectangle lit by
        # a lamp at lattice (20,32); Behind and Emissive rectangles as authored in custom_pass.rs.
        size, k = cls.CUSTOM_PASS_SIZE, cls.CUSTOM_PASS_SIZE//64
        lit = lambda x, y: max(0, round(230-5*((x-20)**2+(y-32)**2)**.5))
        def paint(image, x0, y0, x1, y1, color, skip=lambda x, y: False):
            for py in range(y0*k, y1*k):
                for px in range(x0*k, x1*k):
                    x, y = (px+.5)/k, (py+.5)/k
                    if not skip(x, y):
                        image[(py*size+px)*4:(py*size+px)*4+3] = bytes(int(v) for v in color(x, y))
        off = bytearray([0, 0, 0, 255]*size*size)
        paint(off, 8, 8, 24, 24, lambda x, y: (lit(x, y),)*3)
        on = bytearray(off)
        def rect(x0, y0, x1, y1, fill, border, skip=lambda x, y: False):
            # Border: the 2 units at the min-x (left) and min-y (bottom, i.e. largest lattice y) edges.
            def color(x, y):
                edge = (x < x0+2) or (y >= y1-2)
                if flip_borders:
                    edge = (x >= x1-2) or (y < y0+2)
                return border(x, y) if edge else fill(x, y)
            paint(on, x0, y0, x1, y1, color, skip)
        # The Behind pass draws before the sprite, so the sprite hides the overlap.
        sprite = lambda x, y: 8 <= x < 24 and 8 <= y < 24
        rect(16, 12, 36, 20, lambda x, y: (30, 150, 50), lambda x, y: (200, 40, 160), (lambda x, y: False) if behind_over_sprite else sprite)
        white = (lambda x, y: (255, 255, 255)) if unlit_over else (lambda x, y: (lit(x, y),)*3)
        rect(8, 40, 24, 56, white, lambda x, y: (white(x, y)[0], white(x, y)[0]*.5, white(x, y)[0]*.1))
        rect(52, 50, 62, 58, lambda x, y: (50, 140, 250), lambda x, y: (250, 230, 60))
        rect(15, 28, 25, 36, lambda x, y: (50, 140, 250), lambda x, y: (250, 230, 60))
        if lit_emissive_dimmed:
            paint(on, 52, 50, 62, 58, lambda x, y: (10, 30, 60))
        return bytes(on), bytes(off)

    def test_custom_pass_is_a_maintained_effect_pair(self):
        self.assertIn('CustomPass', parity.EFFECTS)
        for scene in ('BrowserCustomPass', 'BrowserCustomPassOff'):
            self.assertIn(scene, parity.EFFECT_SCENES)
            self.assertIn(scene, parity.MAINTAINED_SCENES)

    def test_custom_pass_stage_ordering_oracles_accept_the_ideal_pair(self):
        on, off = self.custom_pass_pair()
        result = parity.effect_features('CustomPass', on, off, self.CUSTOM_PASS_SIZE, self.CUSTOM_PASS_SIZE)
        self.assertTrue(result['passed'], result['checks'])
        self.assertTrue(parity.feature_checks('BrowserCustomPass', on, self.CUSTOM_PASS_SIZE, self.CUSTOM_PASS_SIZE)['passed'])
        self.assertTrue(parity.feature_checks('BrowserCustomPassOff', off, self.CUSTOM_PASS_SIZE, self.CUSTOM_PASS_SIZE)['passed'])

    def test_custom_pass_stage_ordering_oracles_reject_each_ordering_regression(self):
        cases = dict(
            behind_over_sprite=('behindHiddenBySprite',),
            unlit_over=('overMatchesLitSprite',),
            lit_emissive_dimmed=('emissiveIgnoresLightMap', 'emissiveFullyBrightOnDarkBackground'),
            flip_borders=('behindBorderOnBottomOnly', 'overBorderOnLeftAndBottomOnly', 'emissiveBorderOnLeftAndBottomOnly'))
        for option, failing in cases.items():
            with self.subTest(option=option):
                on, off = self.custom_pass_pair(**{option: True})
                result = parity.effect_features('CustomPass', on, off, self.CUSTOM_PASS_SIZE, self.CUSTOM_PASS_SIZE)
                self.assertFalse(result['passed'])
                for name in failing:
                    self.assertFalse(result['checks'][name], name)
        on, off = self.custom_pass_pair()
        self.assertFalse(parity.effect_features('CustomPass', off, off, self.CUSTOM_PASS_SIZE, self.CUSTOM_PASS_SIZE)['passed'])

    def test_native_custom_pass_cook_compiles_portable_shaders_into_overlay(self):
        parent = parity.ROOT/'.temp/lighting-parity'
        parent.mkdir(parents=True, exist_ok=True)
        pin = __import__('json').loads((parity.ROOT/'tools/experiments/rust-webgpu/toolchain.json').read_text(encoding='utf-8'))['slang']
        commands = []
        def fake_run(command, log, deadline):
            commands.append([str(part) for part in command])
            if '-version' in command:
                log.write_text(pin)
            else:
                Path(command[command.index('-o')+1]).write_bytes(bytes.fromhex('03022307')+bytes(32))
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            project = Path(directory)/'project'
            shaders = project/'assets/shaders/nested'
            shaders.mkdir(parents=True)
            (shaders/'pass.slang').write_text('#include "include/CustomPass.slangh"\n')
            (project/'assets/shaders/native_only.slang').write_text('// not a portable pass\n')
            shim = SimpleNamespace(run=fake_run)
            with patch.dict(sys.modules, web_materials=shim):
                result = parity.native_custom_pass_cook(project, Path(directory), time.monotonic()+30, ['BrowserCustomPass'], slang=Path('slangc'), run_function=fake_run)
                self.assertEqual(parity.native_custom_pass_cook(project, Path(directory), 0, ['BrowserSun'])['status'], 'skipped')
            self.assertEqual(result['status'], 'passed')
            self.assertEqual([item['runtimeUri'] for item in result['compiledShaders']], ['shaders://nested/pass.spv'])
            self.assertTrue((project/'Builds/Intermediate/shaders/nested/pass.spv').is_file())
            compile_command = commands[-1]
            self.assertIn('-fvk-use-scalar-layout', compile_command)
            self.assertEqual(compile_command[compile_command.index('-I')+1], str(parity.ROOT/'src/shaders'))
            self.assertNotIn('AE_WEB_CUSTOM_PASS', ' '.join(compile_command))


if __name__ == '__main__':
    unittest.main()
