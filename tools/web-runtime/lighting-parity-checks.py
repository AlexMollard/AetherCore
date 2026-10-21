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

ROOT = Path(__file__).resolve().parents[2]
SHADOWS = ('Sun', 'Point', 'Spot', 'Mask', 'Skin')
# (32,32) intersects the caster's alpha silhouette. Use the receiver at world
# (0,-1.5), outside the caster's y+-0.5, including the complete probe patch.
SHADOW_RECEIVER_PROBE = (32, 44)
SCENES = ('BrowserDefaultLighting', 'BrowserSolid', 'BrowserGradient', 'BrowserSky', 'BrowserHdr', 'BrowserSun',
          'BrowserPoint', 'BrowserSpot', 'BrowserPbrMaps', 'BrowserLight2D') + tuple(
              'BrowserShadow'+kind+state for kind in SHADOWS for state in ('On', 'Off'))
GRAPHICS = dict(renderScale=1.0, fxaa=False, gtao=False, reflections=False, volumetrics=False,
                bloomStrength=0.0, autoExposure=False, exposure=1.0, tonemap='ACES Filmic',
                gradeContrast=1.0, gradeSaturation=1.0, gradeTemperature=0.0, gradeTint=0.0,
                vignetteIntensity=0.0, chromaticAberration=0.0, filmGrain=0.0,
                motionBlur=0.0, sharpness=0.0, contactShadows=False, anisotropy=1)


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


def native_render_scale_guard(user=None):
    user = user if user is not None else native_user_settings_path()
    value, origin = 1.0, 'private-project.graphics.renderScale'
    if user.is_file():
        settings = tomllib.loads(user.read_text(encoding='utf-8-sig'))
        for table_name, table in settings.items():
            if table_name.lower() == 'graphics' and isinstance(table, dict):
                for key, candidate in table.items():
                    if key.lower() == 'renderscale':
                        value, origin = candidate, 'native-user-override'
    numeric = type(value) in (int, float) and math.isfinite(value)
    effective = min(2.0, max(.25, value)) if numeric else None
    return dict(file=str(user), fileExists=user.is_file(), effectiveValue=effective, origin=origin,
                requestedValue=value if numeric else repr(value),
                qualified=effective == 1)


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
    elif scene.startswith('BrowserShadow') and scene in SCENES:
        checks['receiverRendered'] = max(probes['left']+probes['right']) > 70
    else:
        raise ValueError('No maintained semantic oracle for scene: ' + scene)
    return dict(passed=all(checks.values()), checks=checks, probes=probes, centerProbeSquarePixels=list(center))


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
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--scenes', default=','.join(SCENES), help='Comma-separated maintained scene names; selected scope is recorded')
    parser.add_argument('--timeout', type=int, default=600, help='Shared deadline in seconds, 30..1200')
    parser.add_argument('--playwright', default=os.environ.get('AETHER_PLAYWRIGHT_MODULE'),
                        help='Pinned Playwright package directory; defaults to AETHER_PLAYWRIGHT_MODULE, then Node resolution')
    args = parser.parse_args()
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = dict(schemaVersion=1, operation='lighting-parity', status='failed', cases=[], diagnostics=[],
                  nativeCaptureTick=10, browserCaptureTick=10, nativeCompletionTicks=60,
                  scope='Fixed authored lighting/background scenes; not post-effects or external hosting')
    started, job = time.monotonic(), None
    sys.path.insert(0, str(ROOT/'tools/project'))
    from gameplay_jobs import start_job, status_job, cancel_job, TERMINAL, _OwnedProcess
    try:
        if not 30 <= args.timeout <= 1200:
            raise ValueError('--timeout must be 30..1200')
        scenes = args.scenes.split(',')
        if not scenes or len(set(scenes)) != len(scenes) or any(s not in SCENES for s in scenes):
            raise ValueError('--scenes must contain unique maintained scene names')
        for kind in SHADOWS:
            if sum('BrowserShadow'+kind+state in scenes for state in ('On', 'Off')) == 1:
                raise ValueError('Shadow qualification requires both On and Off scenes: '+kind)
        report['requestedScenes'] = scenes
        # Authored settings cannot override the machine-local scene resolution scale.
        report['nativeRenderScaleGuard'] = native_render_scale_guard()
        if not report['nativeRenderScaleGuard']['qualified']:
            raise ValueError('NATIVE_RENDER_SCALE_UNQUALIFIED: machine-local renderScale must be 1; user settings were not modified')
        directory = args.report.parent/('lighting-parity-'+uuid.uuid4().hex)
        directory.mkdir()
        project = ROOT/'.temp/lighting-parity'/uuid.uuid4().hex
        shutil.copytree(ROOT/'projects/AgentPipelineRust', project, ignore=shutil.ignore_patterns('Builds', 'target', 'obj', 'bin', 'artifacts', '*.scene.bin', '*.prefab.bin'))
        staged = args.build.resolve()/'reference-project'
        for name in ('assets', 'scenes'):
            shutil.copytree(staged/name, project/name, dirs_exist_ok=True)
        manifest = project/'scripts/Cargo.toml'
        manifest.write_text(manifest.read_text(encoding='utf-8').replace('../../../rust/aethercore', (ROOT/'rust/aethercore').as_posix()), encoding='utf-8')
        settings = project/'ProjectSettings.toml'
        original = settings.read_text(encoding='utf-8')
        graphics = '\n[window]\nwidth = 512\nheight = 512\nmode = "windowed"\n[graphics]\n'+'\n'.join(key+' = '+json.dumps(value) for key, value in GRAPHICS.items())+'\n'
        settings.write_text(original+graphics, encoding='utf-8')
        report.update(project=str(project), artifacts=str(directory), graphics=GRAPHICS)
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
        launch = lambda scene: dict(profile='native-desktop-2d-v1', assertions=[dict(entity='$runtime', type='$engine', property='scene', equals=scene)])
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
            source = native_dir/'cases'/str(index)/'reference.png'
            width, height, pixels = png(source)
            destination = directory/(scene+'-native.png')
            shutil.copy2(source, destination)
            captures.append(dict(scene=scene, width=width, height=height, native=str(destination)))
        capture_request = directory/'browser-request.json'
        capture_request.write_text(json.dumps(dict(root=str(args.build.resolve()/'web'), graphics=GRAPHICS, captures=captures,
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
            probes = {key: max(abs(a-b) for a, b in zip(value, web_features['probes'][key])) for key, value in native_features['probes'].items()}
            passed = comparison['passed'] and native_features['passed'] and web_features['passed'] and max(probes.values()) <= 12
            report['cases'].append(dict(scene=capture['scene'], status='passed' if passed else 'failed', width=width, height=height,
                                        comparison=comparison, nativeFeatures=native_features, webFeatures=web_features,
                                        probeMaxChannelErrors=probes, probeTolerance=12))
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
        report['status'] = 'passed' if report['cases'] and all(c['status'] == 'passed' for c in report['cases']) else 'failed'
        if any(pair['status'] != 'passed' for pair in report['shadowPairs']):
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
