"""Compare identical authored lighting scenes on native Vulkan and hardware WebGPU.

Runs one build-once, hidden native test suite, then an owned ephemeral browser server.
Never publishes, changes user settings, or changes the maintained reference project.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import struct
import sys
import time
import tomllib
import uuid
import zlib

from material_effect_oracles import (SCENES as MATERIAL_SCENES, PAIRS as MATERIAL_PAIRS, LIMITS as MATERIAL_LIMITS,
                                     validate_selection as validate_material_selection, scene_features as material_scene_features,
                                     pair_features as material_pair_features)

ROOT = Path(__file__).resolve().parents[2]
SHADOWS = ('Sun', 'Point', 'Spot', 'Mask', 'Skin')
# (32,32) intersects the caster's alpha silhouette. Use the receiver at world
# (0,-1.5), outside the caster's y+-0.5, including the complete probe patch.
SHADOW_RECEIVER_PROBE = (32, 44)
SCENES = ('BrowserDefaultLighting', 'BrowserSolid', 'BrowserGradient', 'BrowserSky', 'BrowserHdr', 'BrowserSun',
          'BrowserPoint', 'BrowserSpot', 'BrowserPbrMaps', 'BrowserLight2D') + tuple(
              'BrowserShadow'+kind+state for kind in SHADOWS for state in ('On', 'Off')) + ('BrowserShadowNoCast',)
GRAPHICS = dict(renderScale=1.0, fxaa=False, gtao=False, reflections=False, volumetrics=False,
                bloomStrength=0.0, autoExposure=False, exposure=1.0, tonemap='ACES Filmic',
                gradeContrast=1.0, gradeSaturation=1.0, gradeTemperature=0.0, gradeTint=0.0,
                vignetteIntensity=0.0, chromaticAberration=0.0, filmGrain=0.0,
                motionBlur=0.0, sharpness=0.0, contactShadows=False, anisotropy=1)

# Keep the qualified Task 3 default scope stable. Task 4 features are explicit
# opt-in until the independent native/Web captures and semantic controls qualify.
EFFECTS = ('ObjectLights', 'Baked', 'Gem', 'Sheen', 'Character', 'Fog', 'Clouds', 'PhysicalSky', 'Darkness', 'Dof',
           'UvScroll', 'VertexColor', 'SkyMesh', 'CustomPass')
EFFECT_SCENES = tuple('Browser'+name+state for name in EFFECTS for state in ('', 'Off') if name != 'Dof') + ('BrowserDofOn', 'BrowserDofOff', 'BrowserParticles2D', 'BrowserParticles3D')
POST_SCENES = ('BrowserPostPattern', 'BrowserPostCorner', 'BrowserPostVolumetric', 'BrowserPostMotion', 'BrowserPostExposure', 'BrowserPostDisplay', 'BrowserPostTonemap')
MAINTAINED_SCENES = tuple(dict.fromkeys(SCENES + EFFECT_SCENES + ('BrowserGraph',) + POST_SCENES + MATERIAL_SCENES))


def graphics_settings(overrides):
    if not isinstance(overrides, dict):
        raise ValueError('--graphics-json requires a JSON object')
    defaults = dict(GRAPHICS, vignetteRoundness=1.0, motionBlurMaxRadius=16.0,
                    gtaoRadius=.5, gtaoStrength=1.0, specularFilter=0.0, foliageSkyLook=0.0,
                    bloomRadius=1.0, exposureKey=.18, exposureSpeed=2.0,
                    reflectionMaxRoughness=.8, reflectionIntensity=1.0, shadowSplitLambda=.9)
    for key, value in overrides.items():
        if key not in defaults:
            raise ValueError('Unknown graphics setting: '+key)
        expected = defaults[key]
        if type(expected) is bool:
            valid = type(value) is bool
        elif type(expected) is str:
            valid = value in ('Reinhard', 'ACES Filmic', 'Uncharted 2', 'Hejl Richard', 'Linear', 'Exponential', 'Filmic Dice', 'Lottes', 'RomBinDaHouse', 'Vanilla', 'AgX')
        else:
            valid = type(value) in (int, float) and math.isfinite(value)
        if not valid:
            raise ValueError('Invalid graphics setting: '+key)
        if key == 'renderScale' and not .25 <= value <= 2:
            raise ValueError('Render-scale qualification requires .25..2')
        if key == 'anisotropy' and (type(value) is not int or value not in (1, 2, 4, 8, 16)):
            raise ValueError('Invalid graphics setting: anisotropy')
    return dict(GRAPHICS, **overrides)


def scheduled_changes(changes, final_extent, initial_extent):
    """Validate launchConfig.changes: graphics/resize steps both backends apply at the same tick.

    A change at tick N lands before simulation update N+1 (the input convention), so it must
    precede the tick-10 capture. Every resize must end at the requested output extent."""
    if not isinstance(changes, list):
        raise ValueError('--changes-json requires a JSON array')
    resized = False
    for change in changes:
        if not isinstance(change, dict) or not set(change) <= {'tick', 'graphics', 'resize'} or not {'graphics', 'resize'} & set(change):
            raise ValueError('A scheduled change is {tick, graphics?, resize?} with graphics or resize')
        if type(change.get('tick')) is not int or not 0 <= change['tick'] <= 9:
            raise ValueError('Scheduled change tick must be 0..9, before the tick-10 capture')
        if 'graphics' in change:
            if not isinstance(change['graphics'], dict) or not change['graphics']:
                raise ValueError('Scheduled graphics must be a non-empty object')
            graphics_settings(change['graphics'])
        if 'resize' in change:
            size = change['resize']
            if not isinstance(size, list) or len(size) != 2 or any(type(v) is not int or not 1 <= v <= 2048 for v in size):
                raise ValueError('Scheduled resize must be [width, height] in 1..2048')
            resized = True
    resizes = [change['resize'] for change in sorted(changes, key=lambda c: c['tick']) if 'resize' in change]
    if resized and resizes[-1] != list(final_extent):
        raise ValueError('The last scheduled resize must reach the requested --extent')
    if resized != (initial_extent != final_extent):
        raise ValueError('--initial-extent must differ from --extent exactly when resizes are scheduled')
    return changes


def native_user_settings_path():
    # Match PlatformPaths::GetUserConfigDir: Windows' actual LocalAppData known
    # folder takes precedence over environment overrides; Unix uses XDG then HOME.
    if os.name == 'nt':
        import ctypes
        folder = ctypes.create_unicode_buffer(32768)
        if ctypes.windll.shell32.SHGetFolderPathW(None, 0x1c, None, 0, folder) == 0 and folder.value:
            root = Path(folder.value)
        elif os.environ.get('LOCALAPPDATA'):
            root = Path(os.environ['LOCALAPPDATA'])
        else:
            raise ValueError('Cannot resolve native LocalAppData settings directory')
    else:
        root = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home()/'.config')
    return root/'AetherCore/UserSettings.toml'


def native_render_scale_guard(user=None, requested=1.0):
    user = user if user is not None else native_user_settings_path()
    value, origin = requested, 'private-project.graphics.renderScale'
    if user.is_file():
        settings = tomllib.loads(user.read_text(encoding='utf-8-sig'))
        for table_name, table in settings.items():
            if table_name.lower() == 'graphics' and isinstance(table, dict):
                for key, candidate in table.items():
                    if key.lower() == 'renderscale':
                        value, origin = candidate, 'native-user-override'
    numeric = type(value) in (int, float) and math.isfinite(value)
    effective = min(2.0, max(.25, value)) if numeric else None
    return dict(file=str(user), fileExists=user.is_file(), effectiveValue=effective, origin=origin,projectRequestedValue=requested,
                requestedValue=value if numeric else repr(value),
                qualified=effective == requested)


def validate_frame_evidence(native, web, width, height, scale=1.0, require_render_extent=False):
    if not isinstance(native,dict):
        raise ValueError('NATIVE_FRAME_EVIDENCE_UNQUALIFIED: missing screenshot .png.json receipt; rebuild native GameRuntime and aether-project')
    if not isinstance(web,dict):
        raise ValueError('WEB_FRAME_EVIDENCE_UNQUALIFIED: missing actual GPU capture receipt; rebuild the browser runtime')
    finite=lambda value:type(value) in (int,float) and math.isfinite(value)
    for backend,evidence in (('NATIVE',native),('WEB',web)):
        if any(type(evidence.get(key)) is not int or evidence[key]<0 for key in ('frameIndex','width','height')):
            raise ValueError(backend+'_FRAME_EVIDENCE_UNQUALIFIED: invalid frame/extent identity')
        if (evidence['width'],evidence['height'])!=(width,height):
            raise ValueError(backend+'_FRAME_EVIDENCE_UNQUALIFIED: receipt and captured output dimensions differ')
        if not finite(evidence.get('exposureUsed')) or evidence['exposureUsed']<0:
            raise ValueError(backend+'_FRAME_EVIDENCE_UNQUALIFIED: finite actual exposure required')
    if not finite(native.get('elapsedTime')) or abs(native['elapsedTime']-10/60)>1e-4:
        raise ValueError('NATIVE_FRAME_EVIDENCE_UNQUALIFIED: captured shader clock must be fixed tick10/60; rebuild native fixed-step runner')
    if type(web.get('temporalFrameIndex')) is not int or native['frameIndex']!=web['temporalFrameIndex'] or web.get('simulationTick')!=10:
        raise ValueError('FRAME_EVIDENCE_UNQUALIFIED: native render identity and Web accepted temporal frame/tick differ')
    if abs(native['exposureUsed']-web['exposureUsed'])>.0002:
        raise ValueError('FRAME_EVIDENCE_UNQUALIFIED: actual native/Web exposure differs')
    expected=[width,height] if scale==1 else [max(64,min(8192,math.floor(value*scale))) for value in (width,height)]
    if require_render_extent or scale!=1:
        for backend,evidence in (('NATIVE',native),('WEB',web)):
            if [evidence.get('renderWidth'),evidence.get('renderHeight')]!=expected:
                raise ValueError(backend+'_RENDER_EXTENT_UNQUALIFIED: actual GPU scene extent does not prove requested ephemeral renderScale; rebuild capture evidence')
    return dict(passed=True,nativeFrameIndex=native['frameIndex'],webTemporalFrameIndex=web['temporalFrameIndex'],
                simulationTick=10,elapsedTime=native['elapsedTime'],exposureUsed=native['exposureUsed'],expectedRenderExtent=expected)


def bake_native(project, directory, deadline, *, worker_factory=None, packer=None):
    project = project.resolve()
    if not project.is_relative_to((ROOT/'.temp/lighting-parity').resolve()):
        raise ValueError('Canonical baking is restricted to this helper\'s private project directory')
    packer = packer or ROOT/('build/default/tools/RelWithDebInfo/AssetPacker.exe' if os.name == 'nt' else 'build/linux-clang/tools/AssetPacker')
    if not packer.is_file():
        raise RuntimeError('Build the native AssetPacker before lighting qualification: '+str(packer))
    if worker_factory is None:
        from gameplay_jobs import _OwnedProcess
        worker_factory = _OwnedProcess
    cooked = lambda path: path.suffix in ('.mesh', '.skel', '.animset', '.texture') or path.name.endswith(('.scene.bin', '.prefab.bin'))
    source_hashes = {path.relative_to(project).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in project.rglob('*') if path.is_file() and not cooked(path)}
    log = directory/'native-bake.log'
    with log.open('wb') as stream:
        worker = worker_factory([str(packer), 'bake-all', str(project)], ROOT, stream)
        try:
            while worker.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Canonical native model bake exceeded shared qualification deadline')
                time.sleep(.03)
            if worker.poll() != 0:
                raise RuntimeError('Native canonical model bake failed; inspect '+str(log))
        finally:
            worker.close()
    for name, digest in source_hashes.items():
        path = project/name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError('Canonical model baking changed an authored source input: '+name)
    artifacts = [dict(path=path.relative_to(project).as_posix(), bytes=path.stat().st_size,
                      sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                 for path in sorted(project.rglob('*')) if path.is_file() and
                 (cooked(path) or path.relative_to(project).as_posix() not in source_hashes)]
    return dict(status='passed', command=[str(packer), 'bake-all', str(project)], log=str(log),
                packerSha256=hashlib.sha256(packer.read_bytes()).hexdigest(),
                authoredSourceHashesUnchanged=True,
                authoredSourceTreeSha256=hashlib.sha256(json.dumps(source_hashes, sort_keys=True).encode('utf-8')).hexdigest(),
                generatedArtifacts=artifacts)


def native_graph_cook(project, build, deadline, scenes, *, compiler=None, slang=None, cook_function=None):
    if 'BrowserGraph' not in scenes:
        return dict(status='skipped', reason='No custom graph scene selected')
    project = project.resolve()
    if not project.is_relative_to((ROOT/'.temp/lighting-parity').resolve()):
        raise ValueError('Native graph cooking is restricted to the private qualification project')
    sys.path.insert(0, str(ROOT/'tools/shaders'))
    from web_materials import cook_native, resolve_compiler
    compiler = resolve_compiler(compiler)
    slang = resolve_slang(build, slang)
    remaining = min(600, int(deadline-time.monotonic()))
    if remaining < 1:
        raise TimeoutError('Shared qualification deadline expired before native graph cooking')
    material = project/'assets/materials/browser-graph.material.toml'
    before = hashlib.sha256(material.read_bytes()).hexdigest()
    artifact = (cook_function or cook_native)(project, material, project/'Builds/Intermediate/shaders', compiler, slang, remaining)
    if hashlib.sha256(material.read_bytes()).hexdigest() != before:
        raise RuntimeError('Native graph cooking changed the authored material')
    return dict(status='passed', authoredSourceUnchanged=True, compiledShader=artifact,
                runtimeUri='shaders://BrowserGraph.spv', overlay='Builds/Intermediate/shaders/',
                qualification='Actual native graph SPIR-V staged; semantic pixel oracle must independently establish execution')


def resolve_slang(build, slang=None):
    if slang is None:
        cache = build/'CMakeCache.txt'
        if cache.is_file():
            for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
                if line.startswith('AETHER_WEB_SLANGC:FILEPATH='):
                    slang = Path(line.split('=', 1)[1])
        slang = slang or Path(shutil.which('slangc') or 'slangc')
    return slang


def native_custom_pass_cook(project, build, deadline, scenes, *, slang=None, run_function=None):
    # The native renderer loads a registered pass as shaders://<stem>.spv from the project's compiled
    # shader overlay (Builds/Intermediate/shaders, which the Editor's ShaderCompiler normally fills).
    # Compile the fixture's portable custom-pass sources exactly like CMake/SlangShaders.cmake does.
    if not any(scene.startswith('BrowserCustomPass') for scene in scenes):
        return dict(status='skipped', reason='No custom pass scene selected')
    project = project.resolve()
    if not project.is_relative_to((ROOT/'.temp/lighting-parity').resolve()):
        raise ValueError('Native custom pass cooking is restricted to the private qualification project')
    sys.path.insert(0, str(ROOT/'tools/shaders'))
    from web_materials import run
    run = run_function or run
    slang = resolve_slang(build, slang)
    source_root = project/'assets/shaders'
    sources = sorted(path for path in source_root.rglob('*.slang') if '#include "include/CustomPass.slangh"' in path.read_text(encoding='utf-8'))
    if not sources:
        raise RuntimeError('Custom pass scene selected but the staged project has no portable custom pass shader')
    output_root = project/'Builds/Intermediate/shaders'
    scratch = project/'Builds/Intermediate/custom-pass-logs'
    scratch.mkdir(parents=True, exist_ok=True)
    version_log = scratch/'slang-version.log'
    run([slang, '-version'], version_log, deadline)
    version = version_log.read_text(encoding='utf-8', errors='replace').strip()
    pin = json.loads((ROOT/'tools/experiments/rust-webgpu/toolchain.json').read_text(encoding='utf-8'))['slang']
    if pin not in version:
        raise ValueError('NATIVE_CUSTOM_PASS_TOOLCHAIN: expected pinned Slang '+pin+'; got '+version)
    compiled = []
    for source in sources:
        stem = source.relative_to(source_root).with_suffix('').as_posix()
        binary = output_root/(stem+'.spv')
        binary.parent.mkdir(parents=True, exist_ok=True)
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        # Exact default AETHERCORE_SLANG_SHADER_ARGS from CMake/SlangShaders.cmake.
        run([slang, source, '-target', 'spirv', '-emit-spirv-directly', '-fvk-use-scalar-layout',
             '-matrix-layout-column-major', '-I', ROOT/'src/shaders', '-o', binary], scratch/(stem.replace('/', '_')+'.log'), deadline)
        data = binary.read_bytes()
        if len(data) < 20 or data[:4] != bytes.fromhex('03022307'):
            raise ValueError('NATIVE_CUSTOM_PASS_SPIRV: compiler did not produce a SPIR-V module for '+stem)
        if hashlib.sha256(source.read_bytes()).hexdigest() != before:
            raise RuntimeError('Native custom pass cooking changed the authored shader: '+stem)
        compiled.append(dict(stem=stem, runtimeUri='shaders://'+stem+'.spv', output=str(binary), sha256=hashlib.sha256(data).hexdigest(), sourceSha256=before))
    return dict(status='passed', slang=version, overlay='Builds/Intermediate/shaders/', compiledShaders=compiled,
                qualification='Actual native SPIR-V staged from the portable fixture; ordering oracles independently establish execution')


def png(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Native capture is not a PNG')
    cursor, compressed = 8, bytearray()
    width = height = channels = None
    while cursor < len(data):
        size = struct.unpack_from('>I', data, cursor)[0]
        kind, payload = data[cursor+4:cursor+8], data[cursor+8:cursor+8+size]
        if len(payload) != size or cursor + size + 12 > len(data):
            raise ValueError('Truncated PNG')
        if zlib.crc32(kind + payload) & 0xffffffff != struct.unpack_from('>I', data, cursor+8+size)[0]:
            raise ValueError('PNG checksum mismatch')
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', payload)
            if depth != 8 or color not in (2, 6) or compression or filtering or interlace or not 1 <= width <= 4096 or not 1 <= height <= 4096:
                raise ValueError('Capture requires a bounded, noninterlaced 8-bit RGB/RGBA PNG')
            channels = 3 if color == 2 else 4
        elif kind == b'IDAT':
            compressed.extend(payload)
        elif kind == b'IEND':
            break
        cursor += size + 12
    if width is None:
        raise ValueError('PNG header missing')
    decoder = zlib.decompressobj()
    stride = width * channels
    raw = decoder.decompress(compressed, height * (stride + 1) + 1)
    if len(raw) != height * (stride + 1) or not decoder.eof:
        raise ValueError('PNG pixel length mismatch')
    output, previous = bytearray(), bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        mode, row = raw[start], bytearray(raw[start+1:start+1+stride])
        if mode > 4:
            raise ValueError('Unknown PNG filter')
        for x in range(stride):
            a, b, c = row[x-channels] if x >= channels else 0, previous[x], previous[x-channels] if x >= channels else 0
            p = a+b-c
            pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
            predictor = 0 if mode == 0 else a if mode == 1 else b if mode == 2 else (a+b)//2 if mode == 3 else a if pa <= pb and pa <= pc else b if pb <= pc else c
            row[x] = (row[x] + predictor) & 255
        for x in range(width):
            output.extend(row[x*channels:x*channels+3])
            output.append(row[x*channels+3] if channels == 4 else 255)
        previous = row
    return width, height, bytes(output)


def patch(image, width, height, x, y, world=True):
    # Fixture probes were authored on a square orthographic camera. Preserve world X
    # when machine-local window preferences give the native capture another aspect.
    u = .5 + (x/64 - .5) / (width/height) if world else x/64
    px, py = round(u * width), round(y/64 * height)
    values = [image[(iy*width+ix)*4:(iy*width+ix)*4+3]
              for iy in range(max(0, py-2), min(height, py+3))
              for ix in range(max(0, px-2), min(width, px+3))]
    if not values:
        raise ValueError('Fixture probe is outside the captured camera')
    return [round(statistics.mean(v[c] for v in values), 3) for c in range(3)]


def compare(native, browser, width, height):
    if len(native) != width*height*4 or len(browser) != len(native):
        raise ValueError('Capture extent/pixel payload mismatch')
    errors = [abs(native[(y*width+x)*4+c] - browser[(y*width+x)*4+c])
              for y in range(0, height, max(1, height//128))
              for x in range(0, width, max(1, width//128)) for c in range(3)]
    errors.sort()
    mean, p95 = statistics.mean(errors), errors[int((len(errors)-1)*.95)]
    return dict(passed=mean <= 8 and p95 <= 18, meanAbsoluteError=mean, p95AbsoluteError=p95,
                maximumAbsoluteError=max(errors), sampledChannels=len(errors),
                tolerances=dict(meanAbsoluteError=8, p95AbsoluteError=18),
                sampling='Regular <=128-per-axis grid, all RGB channels, no edge masking or image alignment')


def feature_checks(scene, image, width, height):
    p = lambda x, y, world=True: patch(image, width, height, x, y, world)
    center = SHADOW_RECEIVER_PROBE if scene.startswith('BrowserShadow') else (32, 32)
    probes = dict(center=p(*center), left=p(16, 32), right=p(48, 32))
    checks = {}
    if scene == 'BrowserDefaultLighting':
        probes['sky'] = p(32, 4, False)
        checks['nativeStartupLightsRoughMesh'] = 80 < probes['center'][0] < 250
        checks['nativeStartupSkyWithoutEnvironment'] = probes['sky'][2] > probes['sky'][1] > probes['sky'][0] > 20
    elif scene == 'BrowserSolid':
        checks['authoredDisplayColor'] = max(abs(a-b) for a, b in zip(probes['center'], [51, 102, 153])) <= 2
    elif scene == 'BrowserGradient':
        probes.update(top=p(32, 4, False), bottom=p(32, 59, False))
        checks['redToBlueGradient'] = probes['top'][0] > 220 and probes['top'][2] < 25 and probes['bottom'][2] > 220 and probes['bottom'][0] < 25
    elif scene == 'BrowserHdr':
        fit = lambda x: (x*(x+.0245786)-.000090537)/(x*(.983729*x+.432951)+.238081)
        expected = [round(255*max(0, min(1, fit(x)))**(1/2.2)) for x in (.5, 8)]
        checks['nativeAcesRadiance'] = all(abs(value-channel) <= 5 for values, channel in zip([probes['left'], probes['right']], expected) for value in values)
        checks['hdrHighlightDistinct'] = probes['right'][0] - probes['left'][0] > 50
    elif scene == 'BrowserSky':
        probes.update(top=p(32, 8, False), bottom=p(32, 55, False))
        checks['authoredBlueAtmosphere'] = probes['top'][2] > probes['top'][1] > probes['top'][0]
        checks['skyGroundDiffer'] = abs(probes['top'][2]-probes['bottom'][2]) > 30
    elif scene == 'BrowserPbrMaps':
        probes.update(base=p(16, 16), normal=p(48, 16), ao=p(16, 48), metal=p(48, 48))
        checks['normalMapAffectsLighting'] = probes['base'][0]-probes['normal'][0] > 10
        checks['occlusionAffectsAmbient'] = probes['base'][0]-probes['ao'][0] > 5
        checks['metalRoughnessAffectsBrdf'] = max(abs(a-b) for a, b in zip(probes['metal'], probes['base'])) > 15
    elif scene == 'BrowserLight2D':
        probes.update(blocked=p(52, 20), open=p(52, 44))
        checks['artworkAlphaShadow'] = probes['open'][0]-probes['blocked'][0] > 25
        checks['radialLightAboveAmbient'] = probes['open'][0] > 50
    elif scene in ('BrowserSun', 'BrowserPoint', 'BrowserSpot'):
        probes['edge'] = p(53, 32)
        checks['finiteDirectLight'] = 100 < probes['center'][0] < 240
        if scene != 'BrowserSun':
            checks['falloffOrCone'] = probes['center'][0]-probes['edge'][0] > (60 if scene == 'BrowserSpot' else 20)
    elif scene == 'BrowserGraph':
        checks['customGraphRedSurface'] = all(red > 70 and red-green > 25 and green-blue > 8
                                              for red, green, blue in (probes['left'], probes['right']))
        checks['customGraphDiffersFromBackground'] = max(abs(a-b) for a,b in zip(probes['center'],p(4,4))) > 40
    elif scene in MATERIAL_SCENES:
        return material_scene_features(scene, image, width, height, p)
    elif scene in POST_SCENES:
        # Detail is a world feature: keep its world X on non-square frames.
        probes.update(corner=p(4,4,False), detail=p(48,24))
        checks['authoredPostSceneVisible'] = max(probes['center']+probes['left']+probes['right']) > 40
        if scene in ('BrowserPostCorner','BrowserPostVolumetric'):
            # Perspective camera y1.5/pitch-8 projects the actual cubes BELOW
            # screen center. The old center/left/right lattice sampled only wall.
            probes.update(warmCube=p(24,36,False),coolCube=p(39,34,False),floor=p(32,52,False),wall=p(32,16,False),
                          warmReflection=p(24,48,False),coolReflection=p(39,46,False),contact=p(23,44,False))
            checks['warmForegroundCubeVisible']=probes['warmCube'][0]-max(probes['warmCube'][1:])>30
            checks['coolForegroundCubeVisible']=probes['coolCube'][2]-max(probes['coolCube'][:2])>30
            checks['floorAndWallBothVisible']=max(probes['floor'])>40 and max(abs(a-b) for a,b in zip(probes['floor'],probes['wall']))>10
        elif scene == 'BrowserPostDisplay':
            background=p(4,4,False)
            checks['displayParticlesVisible']=sum(max(image[(y*width+x)*4+c]-background[c] for c in range(3))>25
                for y in range(height//4,3*height//4) for x in range(width//4,3*width//4))>=8
        elif scene != 'BrowserPostExposure':
            checks['authoredSpatialDetailVisible'] = max(abs(a-b) for a,b in zip(probes['center'],probes['detail'])) > 5
    elif scene in EFFECT_SCENES:
        if 'Particles' in scene:
            background = p(4, 4)
            active = sum(max(image[(y*width+x)*4+c]-background[c] for c in range(3)) > 25
                         for y in range(height//4, 3*height//4) for x in range(width//4, 3*width//4))
            checks['seededBurstVisible'] = active >= 8
            probes['burst'] = p(32, 30)
        elif 'Dof' in scene:
            probes.update(nearSquare=p(15, 32, False), farSquare=p(49, 32, False))
            checks['bothDepthLayersVisible'] = min(probes['nearSquare']+probes['farSquare']) > 160
        elif 'CustomPass' in scene:
            probes.update({name: p(*point) for name, point in CUSTOM_PASS_PROBES.items()})
            checks['coverSpriteVisible'] = max(probes['spriteNear']) > 60
        elif any(name in scene for name in ('Clouds', 'PhysicalSky')):
            probes.update(top=p(32, 8, False), bottom=p(32, 55, False))
            checks['atmosphereVisible'] = max(probes['top']) > 30 and max(abs(a-b) for a,b in zip(probes['top'],probes['bottom'])) > 10
        elif 'Fog' in scene:
            checks['bothReceiversVisible'] = max(probes['left']) > 40 and max(probes['right']) > 40
        elif 'Darkness' in scene:
            checks['uncoveredReceiverVisible'] = max(probes['right']) > 80
        else:
            checks['authoredMaterialVisible'] = max(probes['center']) > 40
            checks['materialDiffersFromBackground'] = max(abs(a-b) for a,b in zip(probes['center'],p(4,4))) > 25
    elif scene.startswith('BrowserShadow') and scene in SCENES:
        checks['receiverRendered'] = max(probes['left']+probes['right']) > 70
        if scene == 'BrowserShadowNoCast':
            probes.update(caster=p(32, 30), clearReceiver=p(20, 30), outsideCaster=p(48, 30))
            checks['disabledCasterVisible'] = probes['outsideCaster'][0]-probes['caster'][0] > 30
            checks['disabledCasterLeavesReceiverLit'] = probes['clearReceiver'][0] > 100
    else:
        raise ValueError('No maintained semantic oracle for scene: ' + scene)
    return dict(passed=all(checks.values()), checks=checks, probes=probes, centerProbeSquarePixels=list(center))


# Maintained 64-lattice probes (x = 32 + 8*worldX, y = 32 - 8*worldY) of BrowserCustomPass. Rectangles (lattice units):
# sprite x8..24 y8..24, behind x16..36 y12..20, over x8..24 y40..56 (mirror of the sprite about the lamp's y = 32),
# emissive dark x52..62 y50..58 and emissive lit x15..25 y28..36. Every rectangle owns a border on its min-x/min-y
# edges only (2 units wide), which is the left and bottom edge on screen.
CUSTOM_PASS_PROBES = dict(
    spriteNear=(20, 22), spriteFar=(12, 13), overNear=(20, 42), overFar=(12, 51),
    overLeft=(9, 46), overBottom=(16, 55), overTop=(16, 41),
    behindCovered=(21, 15), behindFree=(30, 16), behindBottom=(30, 19), behindTop=(30, 13),
    darkFill=(58, 54), darkLeft=(53, 54), darkBottom=(58, 57), darkTop=(58, 51), darkRight=(61, 54),
    litFill=(21, 32), litLeft=(16, 32), litBottom=(21, 35), litTop=(21, 29), litRight=(24, 32))


def custom_pass_features(on, off, width, height):
    # The three stages are qualified by ordering against the engine's own sprite/light-map pipeline, not by
    # absolute colours (those depend on the tonemap): Behind is hidden by the sprite it overlaps, Over equals
    # the lit sprite mirrored about the lamp (same distance, so same light factor), Emissive renders identically
    # on the lamp and outside its radius because the light map never touches it.
    at = lambda image, name: patch(image, width, height, *CUSTOM_PASS_PROBES[name])
    diff = lambda a, b: max(abs(x-y) for x, y in zip(a, b))
    probes = {name: at(on, name) for name in CUSTOM_PASS_PROBES}
    control = {name: at(off, name) for name in CUSTOM_PASS_PROBES}
    green = lambda c: c[1]-max(c[0], c[2])
    tint = lambda c: c[0]-c[2]
    checks = dict(
        behindHiddenBySprite=diff(probes['behindCovered'], control['behindCovered']) <= 4 and max(control['behindCovered']) > 60,
        behindUncoveredPartDrawn=diff(probes['behindFree'], control['behindFree']) > 40 and green(probes['behindFree']) > 15,
        # The light map brightens the covered rows unevenly, so compare hue (green fill vs magenta border), not absolute colour.
        behindBorderOnBottomOnly=probes['behindBottom'][0]-probes['behindBottom'][1] > 20 and green(probes['behindTop']) > 15 and green(probes['behindFree']) > 15 and diff(probes['behindBottom'], control['behindBottom']) > 40,
        overMatchesLitSprite=all(diff(probes[o], control[s]) <= 8 and diff(probes[o], control[o]) > 20
                                 for o, s in (('overNear', 'spriteNear'), ('overFar', 'spriteFar'))),
        overDarkensAwayFromLamp=probes['overNear'][0]-probes['overFar'][0] > 15,
        overBorderOnLeftAndBottomOnly=tint(probes['overLeft']) > 20 and tint(probes['overBottom']) > 20 and tint(probes['overTop']) < 10,
        emissiveIgnoresLightMap=diff(probes['darkFill'], probes['litFill']) <= 6 and diff(probes['darkLeft'], probes['litLeft']) <= 6
                                and diff(probes['darkBottom'], probes['litBottom']) <= 6,
        emissiveFullyBrightOnDarkBackground=max(probes['darkFill'])-max(control['darkFill']) > 100,
        emissiveBorderOnLeftAndBottomOnly=all(diff(probes[k+'Left'], probes[k+'Fill']) > 40 and diff(probes[k+'Bottom'], probes[k+'Fill']) > 40
                                              and diff(probes[k+'Top'], probes[k+'Fill']) <= 8 and diff(probes[k+'Right'], probes[k+'Fill']) <= 8
                                              for k in ('dark', 'lit')))
    return checks, probes


def effect_features(kind, on, off, width, height):
    # Sample authored effect footprint on the maintained 64-square camera lattice.
    # Every pair changes one reflected flag/setting; unchanged images must fail.
    region = range(4,60) if kind in ('Clouds', 'PhysicalSky', 'Dof') else range(23,41) if kind in ('ObjectLights','Baked','Gem','Sheen','Character') else range(12,52)
    differences = [max(abs(a-b) for a,b in zip(patch(on,width,height,x,y,False),patch(off,width,height,x,y,False)))
                   for y in region for x in region]
    changed = sum(delta > 3 for delta in differences)
    checks = dict(authoredEffectChangesPixels=changed >= 4)
    if kind == 'Darkness':
        delta = patch(off,width,height,20,32)[0]-patch(on,width,height,20,32)[0]
        checks['volumeDarkensInterior'] = delta > 15
    if kind == 'Dof':
        # Both squares subtend the same angular width; the near square is exactly
        # at focus2m. Probe just outside the right square's x=59.7 edge: a far lens
        # must spread energy there while the in-focus square keeps its center.
        spread = patch(on,width,height,61,32,False)[0]-patch(off,width,height,61,32,False)[0]
        checks['farLayerSpreadsBeyondSilhouette'] = spread > 3
        checks['focusCenterPreserved'] = abs(patch(on,width,height,15,32,False)[0]-patch(off,width,height,15,32,False)[0]) < 8
    if kind == 'UvScroll':
        # 1.5 UV/s for 1/6 s shifts the 4-band stripe exactly one band: each scrolled
        # band shows the static image's next band, while static neighbours differ.
        bands = [(20, 28), (28, 36), (36, 44)]
        shifted = [max(abs(a-b) for a,b in zip(patch(on,width,height,x0,32),patch(off,width,height,x1,32))) for x0,x1 in bands]
        distinct = [max(abs(a-b) for a,b in zip(patch(off,width,height,x0,32),patch(off,width,height,x1,32))) for x0,x1 in bands]
        checks['scrolledByOneBand'] = max(shifted) < 12 and min(distinct) > 40
    if kind == 'VertexColor':
        # Corners carry red (bottom-left), green (bottom-right) and blue (top-right).
        hue = lambda image, x, y, channel: (lambda c: c[channel]-max(c[i] for i in range(3) if i != channel))(patch(image,width,height,x,y))
        corners = [(19, 45, 0), (45, 45, 1), (45, 19, 2)]
        checks['cornersTakeVertexHues'] = all(hue(on, x, y, c) > 30 for x, y, c in corners)
        checks['controlIgnoresVertexHues'] = all(hue(off, x, y, c) < 15 for x, y, c in corners)
    if kind == 'SkyMesh':
        # The dome sits nearer than the red cube; as sky it is pinned behind it.
        on_center, off_center = patch(on,width,height,32,32), patch(off,width,height,32,32)
        checks['skyDrawsBehindOpaque'] = on_center[0]-on_center[2] > 40
        checks['controlDomeOccludes'] = off_center[2]-off_center[0] > 40
    extra = {}
    if kind == 'CustomPass':
        custom_checks, custom_probes = custom_pass_features(on, off, width, height)
        checks.update(custom_checks)
        extra['customPassProbes'] = custom_probes
    return dict(passed=all(checks.values()), checks=checks, changedProbePixels=changed,
                thresholdExclusive=3, minimumChangedProbePixels=4, maximumProbeDifference=max(differences), **extra)


def shadow_features(kind, on, off, width, height):
    differences = []
    for y in range(20, 44):
        for x in range(4, 29):
            a, b = patch(off, width, height, x, y), patch(on, width, height, x, y)
            differences.append(a[0]-b[0])
    darkened = sum(value > 25 for value in differences)
    checks = dict(receiverDarkened=darkened >= 6)
    result = dict(darkenedProbePixels=darkened, minimumDarkenedProbePixels=6,
                  threshold=25, receiverRoiSquarePixels=dict(x=[4, 28], y=[20, 43]),
                  sample='5x5 patch means on 64x64 world-probe lattice, aspect-corrected')
    if kind in ('Mask', 'Point', 'Spot'):
        upper = patch(off, width, height, 20, 30)[0]-patch(on, width, height, 20, 30)[0]
        lower = patch(off, width, height, 20, 33)[0]-patch(on, width, height, 20, 33)[0]
        checks.update(opaqueUpperCasts=upper > 25, transparentLowerPasses=abs(lower) < 12)
        result.update(upperDifference=upper, lowerDifference=lower,
                      cutoutThresholds=dict(upperMinimumExclusive=25, lowerAbsoluteMaximumExclusive=12))
    return dict(passed=all(checks.values()), checks=checks, **result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/webgpu-browser')
    parser.add_argument('--language', choices=('rust', 'csharp'), default='rust')
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--scenes', default=','.join(SCENES), help='Comma-separated maintained scene names; selected scope is recorded')
    parser.add_argument('--graphics-json', type=Path, help='JSON object file merged over neutral graphics, identically for both backends')
    parser.add_argument('--extent', default='512x512', help='Requested output WIDTHxHEIGHT, both1..2048; actual native extent must match')
    parser.add_argument('--initial-extent', help='Starting WIDTHxHEIGHT when --changes-json resizes to --extent; defaults to --extent')
    parser.add_argument('--changes-json', type=Path, help='JSON array of {tick, graphics?, resize?} applied identically before update tick+1 on both backends')
    parser.add_argument('--isolate-render-scale',action='store_true',help='Apply validated native desktop per-run scale override with persistence disabled; requires GPU source-extent evidence')
    parser.add_argument('--timeout', type=int, default=600, help='Shared deadline in seconds, 30..1200')
    parser.add_argument('--playwright', default=os.environ.get('AETHER_PLAYWRIGHT_MODULE'),
                        help='Pinned Playwright package directory; defaults to AETHER_PLAYWRIGHT_MODULE, then Node resolution')
    args = parser.parse_args()
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = dict(schemaVersion=1, operation='lighting-parity', status='failed', cases=[], diagnostics=[],
                  nativeCaptureTick=10, browserCaptureTick=10, nativeCompletionTicks=60,
                  scope='Explicit authored scene and graphics selection; no unselected effects or external-host qualification')
    started, job = time.monotonic(), None
    sys.path.insert(0, str(ROOT/'tools/project'))
    from gameplay_jobs import start_job, status_job, cancel_job, TERMINAL, _OwnedProcess
    try:
        if not 30 <= args.timeout <= 1200:
            raise ValueError('--timeout must be 30..1200')
        effective_graphics = graphics_settings(json.loads(args.graphics_json.read_text(encoding='utf-8-sig')) if args.graphics_json else {})
        def parse_extent(text, name):
            parts = text.split('x')
            if len(parts) != 2 or any(not value.isdigit() or not 1 <= int(value) <= 2048 for value in parts):
                raise ValueError(name+' must be bounded WIDTHxHEIGHT, both1..2048')
            return tuple(map(int, parts))
        output_width, output_height = parse_extent(args.extent, '--extent')
        initial_width, initial_height = parse_extent(args.initial_extent or args.extent, '--initial-extent')
        changes = scheduled_changes(json.loads(args.changes_json.read_text(encoding='utf-8-sig')) if args.changes_json else [],
                                    (output_width, output_height), (initial_width, initial_height))
        report['requestedOutputExtent']=[output_width,output_height]
        report['initialExtent'] = [initial_width, initial_height]
        report['scheduledChanges'] = changes
        scenes = args.scenes.split(',')
        if not scenes or len(set(scenes)) != len(scenes) or any(s not in MAINTAINED_SCENES for s in scenes):
            raise ValueError('--scenes must contain unique maintained scene names')
        for kind in SHADOWS:
            if sum('BrowserShadow'+kind+state in scenes for state in ('On', 'Off')) == 1:
                raise ValueError('Shadow qualification requires both On and Off scenes: '+kind)
        for kind in EFFECTS:
            on, off = 'Browser'+kind+('On' if kind == 'Dof' else ''), 'Browser'+kind+'Off'
            if (on in scenes) != (off in scenes):
                raise ValueError('Effect qualification requires feature and control scenes: '+kind)
        validate_material_selection(scenes, effective_graphics)
        report['requestedScenes'] = scenes
        if any(scene in MATERIAL_SCENES for scene in scenes):
            report['materialLimits'] = MATERIAL_LIMITS
        # Authored settings cannot override the machine-local scene resolution scale.
        report['nativeRenderScaleGuard'] = native_render_scale_guard(requested=effective_graphics['renderScale'])
        if args.isolate_render_scale:
            before=report['nativeRenderScaleGuard']
            report['nativeRenderScaleGuard']=dict(before,qualified=True,preOverrideQualified=before['qualified'],
                preOverrideEffectiveValue=before['effectiveValue'],effectiveValue=effective_graphics['renderScale'],
                override='validated desktop launchConfig.renderScale; settings persistence disabled')
        if not report['nativeRenderScaleGuard']['qualified']:
            raise ValueError('NATIVE_RENDER_SCALE_UNQUALIFIED: machine-local renderScale overrides the requested project scale; user settings were not modified')
        directory = args.report.parent/('lighting-parity-'+uuid.uuid4().hex)
        directory.mkdir()
        project = ROOT/'.temp/lighting-parity'/uuid.uuid4().hex
        shutil.copytree(ROOT/('projects/AgentPipelineRust' if args.language == 'rust' else 'projects/AgentPipelineCSharp'), project, ignore=shutil.ignore_patterns('Builds', 'target', 'obj', 'bin', 'artifacts', '*.scene.bin', '*.prefab.bin'))
        staged = args.build.resolve()/'reference-project'
        for name in ('assets', 'scenes'):
            shutil.copytree(staged/name, project/name, dirs_exist_ok=True)
        if args.language == 'rust':
            manifest = project/'scripts/Cargo.toml'
            manifest.write_text(manifest.read_text(encoding='utf-8').replace('../../../rust/aethercore', (ROOT/'rust/aethercore').as_posix()), encoding='utf-8')
        else:
            import re
            csproj = project/'scripts/AetherGame.csproj'
            csproj.write_text(re.sub(r'(<ProjectReference\s+Include=")[^"]+AetherCore\.csproj"', lambda m: m[1]+(ROOT/'managed/AetherCore/AetherCore.csproj').as_posix()+'"', csproj.read_text(encoding='utf-8-sig')), encoding='utf-8')
        settings = project/'ProjectSettings.toml'
        original = settings.read_text(encoding='utf-8')
        graphics = f'\n[window]\nwidth = {initial_width}\nheight = {initial_height}\nmode = "windowed"\n[graphics]\n'+'\n'.join(key+' = '+json.dumps(value) for key, value in effective_graphics.items())+'\n'
        settings.write_text(original+graphics, encoding='utf-8')
        report.update(project=str(project), artifacts=str(directory), graphics=effective_graphics)
        report['sceneSha256'] = {s: hashlib.sha256((project/'scenes'/f'{s}.scene.toml').read_bytes()).hexdigest() for s in scenes}
        for scene in scenes:
            maintained = ROOT/'resources/web/reference/scenes'/f'{scene}.scene.toml'
            if (project/'scenes'/f'{scene}.scene.toml').read_bytes() != maintained.read_bytes():
                raise ValueError('STALE_BROWSER_FIXTURE: rebuild the browser fixture before comparing '+scene)
        fixture_files = sorted(path for name in ('assets', 'scenes') for path in (staged/name).rglob('*') if path.is_file())
        tree_hash = hashlib.sha256()
        for path in fixture_files:
            tree_hash.update(path.relative_to(staged).as_posix().encode('utf-8')+b'\0'+hashlib.sha256(path.read_bytes()).digest())
        package = args.build.resolve()/'web/game.pak'
        if package.stat().st_mtime + 1 < max(path.stat().st_mtime for path in fixture_files):
            raise ValueError('STALE_BROWSER_PACKAGE: rebuild game.pak after its authored fixture inputs')
        report['sharedFixtureTreeSha256'] = tree_hash.hexdigest()
        report['browserPackageSha256'] = hashlib.sha256(package.read_bytes()).hexdigest()
        deadline = started+args.timeout
        report['nativeBakeLog'] = str(directory/'native-bake.log')
        report['nativeBake'] = bake_native(project, directory, deadline)
        report['nativeGraphCook'] = native_graph_cook(project, args.build.resolve(), deadline, scenes)
        report['nativeCustomPassCook'] = native_custom_pass_cook(project, args.build.resolve(), deadline, scenes)
        launch = lambda scene: dict(profile='native-desktop-2d-v1', assertions=[dict(entity='$runtime', type='$engine', property='scene', equals=scene)],
            **({'renderScale':effective_graphics['renderScale']} if args.isolate_render_scale else {}), **({'changes': changes} if changes else {}))
        request = dict(project=str(project), operation='test', target='desktop', ticks=60, step=1/60, scene=scenes[0], timeoutMs=min(600000,args.timeout*1000),
                       launchConfig=launch(scenes[0]), cases=[dict(name=s, scene=s, launchConfig=launch(s)) for s in scenes])
        job = start_job(request, repo=ROOT)
        while job['report']['status'] not in TERMINAL:
            if time.monotonic() >= deadline:
                raise TimeoutError('Shared native/browser qualification deadline exceeded')
            time.sleep(.05)
            job = status_job(job['jobId'], repo=ROOT)
        report['nativeJob'] = job
        if job['report']['status'] != 'passed':
            raise RuntimeError('Native fixture suite failed; inspect nativeJob report and runtime logs')
        native_dir = Path(job['reportPath']).parent
        captures = []
        for index, scene in enumerate(scenes):
            runtime_log = native_dir/'cases'/str(index)/'runtime.log'
            runtime_text = runtime_log.read_text(encoding='utf-8', errors='replace')
            if any(marker in runtime_text.lower() for marker in ('invalid mesh magic', 'failed to load gltf', 'failed to load model')):
                raise RuntimeError('Native model load failed despite completed gameplay assertions; inspect '+str(runtime_log))
            if scene == 'BrowserGraph' and any(marker in runtime_text.lower() for marker in ('pipeline build failed', 'failed to load shader', 'failed to read shader')):
                raise RuntimeError('Native custom graph shader failed; inspect '+str(runtime_log))
            if scene.startswith('BrowserCustomPass') and any(marker in runtime_text.lower() for marker in ('failed to create pipeline for', 'failed to load shader', 'failed to read shader')):
                raise RuntimeError('Native custom pass shader failed; inspect '+str(runtime_log))
            source = native_dir/'cases'/str(index)/'reference.png'
            width, height, pixels = png(source)
            if (width,height)!=(output_width,output_height):
                raise ValueError('NATIVE_OUTPUT_EXTENT_UNQUALIFIED: actual native output differs from requested extent; user settings were not modified')
            destination = directory/(scene+'-native.png')
            shutil.copy2(source, destination)
            evidence_path=Path(str(source)+'.json')
            evidence=json.loads(evidence_path.read_text(encoding='utf-8')) if evidence_path.is_file() else None
            if not isinstance(evidence,dict):
                raise ValueError('NATIVE_FRAME_EVIDENCE_UNQUALIFIED: missing screenshot receipt; rebuild native GameRuntime/aether-project before qualification')
            Path(str(destination)+'.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
            captures.append(dict(scene=scene, width=width, height=height, native=str(destination),nativeFrameEvidence=evidence))
        capture_request = directory/'browser-request.json'
        capture_request.write_text(json.dumps(dict(root=str(args.build.resolve()/'web'), language=args.language, graphics=effective_graphics, captures=captures,
                                                  changes=changes, initialExtent=[initial_width, initial_height],
                                                  playwright=args.playwright)), encoding='utf-8')
        browser_report = directory/'browser.json'
        with (directory/'browser.log').open('wb') as stream:
            worker = _OwnedProcess([shutil.which('node') or 'node', str(Path(__file__).with_suffix('.mjs')), '--request='+str(capture_request), '--report='+str(browser_report)], ROOT, stream)
            try:
                while worker.poll() is None:
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Shared native/browser qualification deadline exceeded')
                    time.sleep(.03)
                if worker.poll() != 0:
                    if browser_report.is_file():
                        report['browser'] = json.loads(browser_report.read_text(encoding='utf-8'))
                    raise RuntimeError('Browser capture failed; inspect '+str(browser_report))
            finally:
                worker.close()
        report['browser'] = json.loads(browser_report.read_text(encoding='utf-8'))
        for capture in captures:
            width, height, native = png(Path(capture['native']))
            browser = (directory/(capture['scene']+'-web.rgba')).read_bytes()
            comparison = compare(native, browser, width, height)
            native_features = feature_checks(capture['scene'], native, width, height)
            web_features = feature_checks(capture['scene'], browser, width, height)
            web_evidence=next((item.get('frameEvidence') for item in report['browser']['captures'] if item['scene']==capture['scene']),None)
            evidence_validation=validate_frame_evidence(capture['nativeFrameEvidence'],web_evidence,width,height,effective_graphics['renderScale'],args.isolate_render_scale)
            probes = {key: max(abs(a-b) for a, b in zip(value, web_features['probes'][key])) for key, value in native_features['probes'].items()}
            passed = comparison['passed'] and native_features['passed'] and web_features['passed'] and max(probes.values()) <= 12
            report['cases'].append(dict(scene=capture['scene'], status='passed' if passed else 'failed', width=width, height=height,
                                        comparison=comparison, nativeFeatures=native_features, webFeatures=web_features,
                                        probeMaxChannelErrors=probes, probeTolerance=12,nativeFrameEvidence=capture['nativeFrameEvidence'],
                                        webFrameEvidence=web_evidence,frameEvidenceValidation=evidence_validation))
        report['shadowPairs'] = []
        for kind in SHADOWS:
            on_name, off_name = 'BrowserShadow'+kind+'On', 'BrowserShadow'+kind+'Off'
            if on_name not in scenes:
                continue
            on_capture = next(c for c in captures if c['scene'] == on_name)
            off_capture = next(c for c in captures if c['scene'] == off_name)
            width, height, native_on = png(Path(on_capture['native']))
            off_width, off_height, native_off = png(Path(off_capture['native']))
            if (width, height) != (off_width, off_height):
                raise ValueError('Native shadow pair capture dimensions differ: '+kind)
            native_pair = shadow_features(kind, native_on, native_off, width, height)
            web_pair = shadow_features(kind, (directory/(on_name+'-web.rgba')).read_bytes(),
                                       (directory/(off_name+'-web.rgba')).read_bytes(), width, height)
            report['shadowPairs'].append(dict(feature=kind, native=native_pair, web=web_pair,
                                              status='passed' if native_pair['passed'] and web_pair['passed'] else 'failed'))
        if all(scene in scenes for scene in ('BrowserShadowMaskOn', 'BrowserShadowMaskOff', 'BrowserShadowNoCast')):
            captures_by_scene = {capture['scene']: capture for capture in captures}
            native_images = {scene: png(Path(captures_by_scene[scene]['native'])) for scene in ('BrowserShadowMaskOn', 'BrowserShadowMaskOff', 'BrowserShadowNoCast')}
            width, height, _ = native_images['BrowserShadowNoCast']
            no_cast_results = {}
            for backend in ('native', 'web'):
                images = {scene: native_images[scene][2] if backend == 'native' else (directory/(scene+'-web.rgba')).read_bytes() for scene in native_images}
                on, disabled, off = images['BrowserShadowMaskOn'], images['BrowserShadowNoCast'], images['BrowserShadowMaskOff']
                delta = patch(disabled, width, height, 20, 30)[0]-patch(on, width, height, 20, 30)[0]
                unchanged_receiver = max(abs(a-b) for a, b in zip(patch(disabled, width, height, 20, 30), patch(off, width, height, 20, 30)))
                checks = dict(castingFlagRemovesShadow=delta > 25, disabledCasterMatchesUnshadowedReceiver=unchanged_receiver < 12)
                no_cast_results[backend] = dict(passed=all(checks.values()), checks=checks, removedShadowDifference=delta, unshadowedReceiverError=unchanged_receiver)
            report['shadowPairs'].append(dict(feature='NoCast', **no_cast_results,
                                              status='passed' if all(value['passed'] for value in no_cast_results.values()) else 'failed'))
        report['effectPairs'] = []
        for kind in EFFECTS:
            on_name, off_name = 'Browser'+kind+('On' if kind == 'Dof' else ''), 'Browser'+kind+'Off'
            if on_name not in scenes:
                continue
            on_capture = next(c for c in captures if c['scene'] == on_name)
            off_capture = next(c for c in captures if c['scene'] == off_name)
            width, height, native_on = png(Path(on_capture['native']))
            off_width, off_height, native_off = png(Path(off_capture['native']))
            if (width,height) != (off_width,off_height):
                raise ValueError('Effect control capture dimensions differ: '+kind)
            native_pair = effect_features(kind,native_on,native_off,width,height)
            web_pair = effect_features(kind,(directory/(on_name+'-web.rgba')).read_bytes(),(directory/(off_name+'-web.rgba')).read_bytes(),width,height)
            report['effectPairs'].append(dict(feature=kind,native=native_pair,web=web_pair,
                                             status='passed' if native_pair['passed'] and web_pair['passed'] else 'failed'))
        report['materialPairs'] = []
        for pair in MATERIAL_PAIRS:
            if pair['on'] not in scenes:
                continue
            on_capture = next(c for c in captures if c['scene'] == pair['on'])
            off_capture = next(c for c in captures if c['scene'] == pair['off'])
            width, height, native_on = png(Path(on_capture['native']))
            off_width, off_height, native_off = png(Path(off_capture['native']))
            if (width, height) != (off_width, off_height):
                raise ValueError('Material control capture dimensions differ: ' + pair['name'])
            native_pair = material_pair_features(pair, native_on, native_off, width, height)
            web_pair = material_pair_features(pair, (directory/(pair['on']+'-web.rgba')).read_bytes(),
                                               (directory/(pair['off']+'-web.rgba')).read_bytes(), width, height)
            report['materialPairs'].append(dict(feature=pair['name'], native=native_pair, web=web_pair,
                                               status='passed' if native_pair['passed'] and web_pair['passed'] else 'failed'))
        report['status'] = 'passed' if report['cases'] and all(c['status'] == 'passed' for c in report['cases']) else 'failed'
        if any(pair['status'] != 'passed' for pair in report['shadowPairs']):
            report['status'] = 'failed'
        if any(pair['status'] != 'passed' for pair in report['effectPairs']):
            report['status'] = 'failed'
        if any(pair['status'] != 'passed' for pair in report['materialPairs']):
            report['status'] = 'failed'
    except (Exception, KeyboardInterrupt) as error:
        report['diagnostics'].append(dict(code='LIGHTING_PARITY', message=str(error)))
    finally:
        if job and job['report']['status'] not in TERMINAL:
            cancel_job(job['jobId'], repo=ROOT)
            cleanup_deadline = time.monotonic()+15
            while job['report']['status'] not in TERMINAL and time.monotonic()<cleanup_deadline:
                time.sleep(.05)
                job = status_job(job['jobId'], repo=ROOT)
            report['nativeCancellationCompleted'] = job['report']['status'] in TERMINAL
            if not report['nativeCancellationCompleted']:
                report['diagnostics'].append(dict(code='LIGHTING_NATIVE_CLEANUP', message='Native supervisor has not acknowledged process retirement; inspect nativeJob'))
        report['durationMs'] = int((time.monotonic()-started)*1000)
        args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(status=report['status'], report=str(args.report))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
