"""Compare identical authored gameplay scenes' observable state on the native desktop runner and the browser.

For each maintained scene both backends run the same fixed-step ticks with the same scheduled input, and
at each checkpoint tick every named entity's world transform (position/rotation/scale) and script-published
properties are diffed. No images are captured. Runs one owned native job, then one owned ephemeral browser
server under a shared deadline, in a private project copy; the maintained projects are never modified.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
import time
import tomllib
import uuid

ROOT = Path(__file__).resolve().parents[2]
LANGUAGES = {'rust': 'AgentPipelineRust', 'csharp': 'AgentPipelineCSharp'}
CHECKPOINTS = (1, 10, 30, 60)
TOLERANCE = 1e-5
# The native checkpoint counter must equal completed simulation steps; each run verifies it through
# $runtime.simulatedTicks rather than assuming it.
NATIVE_WARMUP_UPDATES = 0
# Same events the maintained reference fixture feeds the native runner (tools/project/reference_fixture.py).
REFERENCE_INPUT = [dict(tick=0, key='space', down=True), dict(tick=20, key='space', down=False)]
# Arrow keys walk the character-controller fixture; space jumps (edge-triggered by the probe).
CHARACTER_INPUT = [dict(tick=2, key='right', down=True), dict(tick=30, key='space', down=True), dict(tick=31, key='space', down=False),
                   dict(tick=100, key='space', down=True), dict(tick=101, key='space', down=False), dict(tick=120, key='right', down=False),
                   dict(tick=125, key='up', down=True), dict(tick=135, key='up', down=False)]
# 3D fixtures (resources/web/reference/scenes/Browser*3D*.scene.toml, scripts in both language projects). Run them with
# --checkpoints 1,10,30,60,120,150 so physics settling, the late jump and the scene transition are all observed.
SCENES = {'ReferenceStart': REFERENCE_INPUT, 'BrowserPhysics3D': [], 'BrowserBodies3D': [], 'BrowserRaycast3D': [], 'BrowserContacts3D': [],
          'BrowserCharacter3D': CHARACTER_INPUT, 'BrowserRagdoll3D': [], 'BrowserSkinClips3D': [], 'BrowserPrefab3D': [], 'BrowserTransition3D': []}
# Native Input::KeyCodeFromName subset; the browser bridge indexes keys by the same codes.
NAMED_KEYS = {'space': 32, 'enter': 257, 'return': 257, 'escape': 256, 'esc': 256, 'tab': 258,
              'left': 263, 'right': 262, 'up': 265, 'down': 264}


def key_code(name):
    name = name.lower()
    if len(name) == 1 and 'a' <= name <= 'z':
        return 65 + ord(name) - ord('a')
    if len(name) == 1 and '0' <= name <= '9':
        return 48 + ord(name) - ord('0')
    if name in NAMED_KEYS:
        return NAMED_KEYS[name]
    raise ValueError('Unsupported input key: ' + name)


def validate_checkpoints(ticks):
    if not isinstance(ticks, (list, tuple)) or not ticks:
        raise ValueError('Checkpoints must be a non-empty list of ticks')
    if any(type(t) is not int or not 1 <= t <= 600 for t in ticks):
        raise ValueError('Checkpoint ticks must be integers in 1..600')
    if len(set(ticks)) != len(ticks):
        raise ValueError('Checkpoint ticks must be unique')
    return sorted(ticks)


def validate_tolerance(value):
    if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
        raise ValueError('Tolerance must be a finite number >= 0')
    return float(value)


def validate_input(events, last_tick):
    if not isinstance(events, list):
        raise ValueError('Input must be a JSON array')
    for event in events:
        if not isinstance(event, dict) or set(event) != {'tick', 'key', 'down'}:
            raise ValueError('An input event is {tick, key, down}')
        if type(event['tick']) is not int or not 0 <= event['tick'] <= last_tick:
            raise ValueError('Input tick must be 0..last checkpoint')
        if type(event['down']) is not bool or not isinstance(event['key'], str):
            raise ValueError('Input key must be a string and down a boolean')
        key_code(event['key'])
    return events


def transform_of(matrix):
    """Decompose a column-major localToWorld into position, scale, canonical (w>=0) quaternion x,y,z,w."""
    if not isinstance(matrix, list) or len(matrix) != 16 or any(type(v) not in (int, float) for v in matrix):
        raise ValueError('localToWorld must be 16 numbers')
    columns = [matrix[i*4:i*4+3] for i in range(3)]
    scale = [math.sqrt(sum(v*v for v in c)) for c in columns]
    basis = [[v/s if s else 0.0 for v in c] for c, s in zip(columns, scale)]
    m = lambda row, column: basis[column][row]
    trace = m(0, 0)+m(1, 1)+m(2, 2)
    if trace > 0:
        s = math.sqrt(trace+1)*2
        q = [(m(2, 1)-m(1, 2))/s, (m(0, 2)-m(2, 0))/s, (m(1, 0)-m(0, 1))/s, s/4]
    elif m(0, 0) > m(1, 1) and m(0, 0) > m(2, 2):
        s = math.sqrt(1+m(0, 0)-m(1, 1)-m(2, 2))*2
        q = [s/4, (m(0, 1)+m(1, 0))/s, (m(0, 2)+m(2, 0))/s, (m(2, 1)-m(1, 2))/s]
    elif m(1, 1) > m(2, 2):
        s = math.sqrt(1+m(1, 1)-m(0, 0)-m(2, 2))*2
        q = [(m(0, 1)+m(1, 0))/s, s/4, (m(1, 2)+m(2, 1))/s, (m(0, 2)-m(2, 0))/s]
    else:
        s = math.sqrt(1+m(2, 2)-m(0, 0)-m(1, 1))*2
        q = [(m(0, 2)+m(2, 0))/s, (m(1, 2)+m(2, 1))/s, s/4, (m(1, 0)-m(0, 1))/s]
    if q[3] < 0 or (q[3] == 0 and next((v for v in q if v), 0) < 0):
        q = [-v for v in q]
    return dict(position=[matrix[12], matrix[13], matrix[14]], rotation=q, scale=scale)


def normalize(state, scene=None):
    """Flatten a native state.json / browser entities() snapshot into {(entity, type): [properties,...]}."""
    records = {}
    for record in state['entities']:
        properties = dict(record['properties'])
        if record['entity'] == '$runtime':
            properties = {k: v for k, v in properties.items() if k == 'scene'}
        elif record['type'] == '$engine' and 'localToWorld' in properties:
            properties.update(transform_of(properties.pop('localToWorld')))
            properties.pop('position', None)
            properties['position'] = transform_of(record['properties']['localToWorld'])['position']
        records.setdefault((record['entity'], record['type']), []).append(properties)
    if scene is not None:
        records.setdefault(('$runtime', '$engine'), []).append(dict(scene=scene))
    return records


def transforms(records):
    return {entity: {key: props[key] for key in ('position', 'rotation', 'scale') if key in props}
            for (entity, kind), occurrences in records.items() if kind == '$engine'
            for props in occurrences[:1] if 'position' in props}


def _numbers_match(native, web, tolerance):
    return abs(native-web) <= tolerance


def _compare_value(field, native, web, tolerance, out):
    both_numbers = all(type(v) in (int, float) for v in (native, web))
    if both_numbers:
        exact = type(native) is int and type(web) is int
        delta = abs(native-web)
        if (exact and native != web) or (not exact and not _numbers_match(native, web, tolerance)):
            out.append(dict(field=field, native=native, web=web, delta=delta))
    elif isinstance(native, list) and isinstance(web, list) and len(native) == len(web):
        for index, (a, b) in enumerate(zip(native, web)):
            _compare_value(f'{field}[{index}]', a, b, tolerance, out)
    elif native != web or type(native) is not type(web):
        out.append(dict(field=field, native=native, web=web))


AXES = {'position': 'xyz', 'scale': 'xyz', 'rotation': 'xyzw'}


def compare_records(tick, native, web, tolerance):
    """Diff two normalized snapshots. Returns (mismatches, fieldsCompared); every mismatch names entity, tick, field, native, web."""
    mismatches, compared = [], 0
    for key in sorted(set(native)|set(web)):
        entity, kind = key
        left, right = native.get(key, []), web.get(key, [])
        if len(left) != len(right) or not left or not right:
            mismatches.append(dict(entity=entity, type=kind, tick=tick, field='exists' if kind == '$engine' else 'instances',
                                   native=len(left) > 0 if kind == '$engine' else len(left),
                                   web=len(right) > 0 if kind == '$engine' else len(right)))
            continue
        for index, (a, b) in enumerate(zip(left, right)):
            found = []
            for name in sorted(set(a)|set(b)):
                if name not in a or name not in b:
                    found.append(dict(field=name, native=a.get(name, '<absent>'), web=b.get(name, '<absent>')))
                elif name in AXES and isinstance(a[name], list) and isinstance(b[name], list) and len(a[name]) == len(b[name]) == len(AXES[name]):
                    for axis, x, y in zip(AXES[name], a[name], b[name]):
                        _compare_value(f'{name}.{axis}', x, y, tolerance, found)
                else:
                    _compare_value(name, a[name], b[name], tolerance, found)
            compared += len(set(a)|set(b))+sum(len(AXES[n])-1 for n in AXES if n in a)
            for item in found:
                if len(left) > 1:
                    item['field'] = f'[{index}].{item["field"]}'
                mismatches.append(dict(entity=entity, type=kind, tick=tick, **item))
    return mismatches, compared


def compare_case(scene, ticks, native_states, web_states, tolerance):
    """native_states: {tick: native state.json}; web_states: {tick: aetherTest.entities()} -> per-checkpoint report entries."""
    checkpoints = []
    for tick in ticks:
        entry = dict(tick=tick)
        native, web = native_states.get(tick), web_states.get(tick)
        if native is None or web is None:
            entry.update(status='failed', mismatches=[dict(entity='*', type='*', tick=tick, field='checkpoint',
                                                           native=native is not None, web=web is not None)],
                         fieldsCompared=0)
        else:
            left, right = normalize(native), normalize(web, web.get('scene'))
            mismatches, compared = compare_records(tick, left, right, tolerance)
            entry.update(status='passed' if not mismatches else 'failed', mismatches=mismatches, fieldsCompared=compared,
                         entitiesCompared=len(set(left)&set(right)),
                         nativeTransforms=transforms(left), webTransforms=transforms(right))
        checkpoints.append(entry)
    return dict(scene=scene, status='passed' if checkpoints and all(c['status'] == 'passed' for c in checkpoints) else 'failed',
                tolerance=tolerance, checkpoints=checkpoints)


def perturb(web_states, amount):
    """Shift the first $engine entity's world X in a copy of the web states; used as a harness-sensitivity control."""
    states = copy.deepcopy(web_states)
    for state in states.values():
        for record in state['entities']:
            if record['type'] == '$engine' and 'localToWorld' in record['properties']:
                record['properties']['localToWorld'][12] += amount
                return states
    return states

# 3D probe scripts live outside each project's scripts/ directory (projects/*/fixtures3d) because the 2D production
# publisher scans everything under scripts/. Staging injects them into the private project only.
FIXTURE_3D_SCRIPTS = ('Bodies3DProbe', 'Raycast3DProbe', 'Contacts3DProbe', 'Walker3DProbe', 'Ragdoll3DProbe', 'Skin3DProbe',
                      'Spawner3DProbe', 'Transition3DProbe', 'TransitionResult3DProbe', 'TemporalCutProbe', 'TemporalSwitchProbe')


def inject_3d_fixtures(language, project):
    source = ROOT/'projects'/LANGUAGES[language]/'fixtures3d'
    if language == 'csharp':
        shutil.copy2(source/'Gameplay3D.cs', project/'scripts/Gameplay3D.cs')
        return
    shutil.copy2(source/'gameplay3d.rs', project/'scripts/src/gameplay3d.rs')
    lib = project/'scripts/src/lib.rs'
    names = ', '.join(FIXTURE_3D_SCRIPTS)
    text = lib.read_text(encoding='utf-8').replace('mod custom_pass;', 'mod gameplay3d;\nuse gameplay3d::{'+names+'};\nmod custom_pass;', 1)
    text, count = re.subn(r'(aethercore::export_gameplay!\()([^)]*)\)', lambda m: m[1]+m[2].rstrip()+', '+names+')', text, count=1)
    if count != 1 or 'mod gameplay3d;' not in text:
        raise ValueError('The Rust project lib.rs no longer has the expected module/export_gameplay! layout for 3D fixture injection')
    lib.write_text(text, encoding='utf-8')


def stage_project(language, scenes, build, directory_root, fixtures_3d=False):
    project = directory_root/uuid.uuid4().hex
    shutil.copytree(ROOT/'projects'/LANGUAGES[language], project,
                    ignore=shutil.ignore_patterns('Builds', 'target', 'obj', 'bin', 'artifacts', '*.scene.bin', '*.prefab.bin', '.aether'))
    staged = build/'reference-project'
    for name in ('assets', 'scenes', 'animations'):
        if (staged/name).is_dir():
            shutil.copytree(staged/name, project/name, dirs_exist_ok=True)
    manifest = project/'scripts/Cargo.toml'
    if language == 'rust' and manifest.is_file():
        manifest.write_text(manifest.read_text(encoding='utf-8').replace('../../../rust/aethercore', (ROOT/'rust/aethercore').as_posix()), encoding='utf-8')
    csproj = project/'scripts/AetherGame.csproj'
    if language == 'csharp' and csproj.is_file():
        # The staged copy lives outside projects/, so its relative engine reference no longer resolves.
        csproj.write_text(re.sub(r'(<ProjectReference\s+Include=")[^"]+AetherCore\.csproj"', lambda m: m[1]+(ROOT/'managed/AetherCore/AetherCore.csproj').as_posix()+'"',
                                 csproj.read_text(encoding='utf-8-sig')), encoding='utf-8')
    if fixtures_3d:
        inject_3d_fixtures(language, project)
    for scene in scenes:
        if not (project/'scenes'/f'{scene}.scene.toml').is_file():
            raise ValueError('Scene is not staged for the private project: '+scene)
        browser_copy = staged/'scenes'/f'{scene}.scene.toml'
        if browser_copy.is_file() and browser_copy.read_bytes() != (project/'scenes'/f'{scene}.scene.toml').read_bytes():
            raise ValueError('STALE_BROWSER_FIXTURE: rebuild the browser fixture before comparing '+scene)
    return project


def scene_kind(project, scene):
    return tomllib.loads((project/'scenes'/f'{scene}.scene.toml').read_text(encoding='utf-8-sig')).get('scene', {}).get('kind', '2d')


def simulated_ticks(state):
    for record in state['entities']:
        if record['entity'] == '$runtime' and 'simulatedTicks' in record['properties']:
            return record['properties']['simulatedTicks']
    return None


def select_native(states, ticks):
    """Map each simulated tick N to the native checkpoint whose $runtime.simulatedTicks == N.

    The native runner's checkpoint counter counts layer updates, which includes warm-up updates that
    simulate nothing, so checkpoint K does not mean K simulation steps. Native records neighbouring
    checkpoints (N..N+1); the one that actually completed N simulation steps is the comparand.
    Returns ({N: state or None}, {N: native checkpoint tick or None})."""
    selected, mapping = {}, {}
    for tick in ticks:
        found = [(k, s) for k, s in sorted(states.items()) if tick <= k <= tick+1 and simulated_ticks(s) == tick]
        selected[tick], mapping[tick] = (found[0][1], found[0][0]) if found else (None, None)
    return selected, mapping


def native_checkpoint_ticks(ticks):
    return sorted({t+offset for t in ticks for offset in (0, 1)})


def native_launch(scene_input, ticks):
    # Checkpoint assertions are mandatory in the native runner; this one holds for any value so the
    # run records state without asserting it. The comparison itself is the oracle.
    trivial = lambda: dict(entity='$runtime', type='$engine', property='audioStarted', equals=0, tolerance=1e9)
    return dict(profile='native-desktop-2d-v1', readSaves=True, input=scene_input,
                checkpoints=[dict(tick=t, assertions=[trivial()]) for t in ticks], assertions=[trivial()])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--language', choices=sorted(LANGUAGES), default='rust')
    parser.add_argument('--build', type=Path, default=ROOT/'build/webgpu-browser')
    parser.add_argument('--web-root', type=Path, help='Served browser export; defaults to <build>/web. Required for a managed C# export')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--stage-project', action='store_true',
                        help='Only stage the private project for --language (project, reference scenes/assets and 3D fixture scripts) and print its path')
    parser.add_argument('--scenes', default='ReferenceStart', help='Comma-separated maintained scene names: '+', '.join(SCENES))
    parser.add_argument('--checkpoints', default=','.join(map(str, CHECKPOINTS)), help='Comma-separated completed-tick checkpoints (1..600)')
    parser.add_argument('--tolerance', type=float, default=TOLERANCE, help='Absolute tolerance for positions/rotations/scales/float properties')
    parser.add_argument('--input-json', type=Path, help='JSON array of {tick,key,down} overriding every scene\'s default input')
    parser.add_argument('--timeout', type=int, default=600, help='Shared deadline in seconds, 30..1200')
    parser.add_argument('--playwright', default=os.environ.get('AETHER_PLAYWRIGHT_MODULE'))
    args = parser.parse_args()
    if args.stage_project:
        print(json.dumps(dict(project=str(stage_project(args.language, [], args.build.resolve(), ROOT/'.temp/gameplay-parity', fixtures_3d=True)))))
        return 0
    if args.report is None:
        parser.error('--report is required unless --stage-project is given')
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = dict(schemaVersion=1, operation='gameplay-parity', status='failed', language=args.language, cases=[], diagnostics=[],
                  artifacts=None, scope='Named-entity world transforms and script properties at checkpoint ticks; no pixels')
    started, job = time.monotonic(), None
    sys.path.insert(0, str(ROOT/'tools/project'))
    from gameplay_jobs import start_job, status_job, cancel_job, TERMINAL, _OwnedProcess
    try:
        if not 30 <= args.timeout <= 1200:
            raise ValueError('--timeout must be 30..1200')
        tolerance = validate_tolerance(args.tolerance)
        ticks = validate_checkpoints([int(v) for v in args.checkpoints.split(',')])
        scenes = args.scenes.split(',')
        if not scenes or len(set(scenes)) != len(scenes) or any(s not in SCENES for s in scenes):
            raise ValueError('--scenes must contain unique maintained scene names: '+', '.join(SCENES))
        override = validate_input(json.loads(args.input_json.read_text(encoding='utf-8-sig')), ticks[-1]) if args.input_json else None
        inputs = {s: validate_input(copy.deepcopy(override if override is not None else SCENES[s]), ticks[-1]) for s in scenes}
        report.update(tolerance=tolerance, tolerancePolicy='absolute, inclusive; rotation as canonical (w>=0) quaternion', checkpoints=ticks,
                      requestedScenes=scenes, input=inputs)
        build = args.build.resolve()
        web_root = (args.web_root or build/'web').resolve()
        if not (web_root/'index.html').is_file():
            raise ValueError('Browser export not found: '+str(web_root/'index.html'))
        directory = args.report.parent/('gameplay-parity-'+uuid.uuid4().hex)
        directory.mkdir()
        reference_scenes = build/'reference-project/scenes'
        fixtures_3d = any(tomllib.loads((reference_scenes/f'{s}.scene.toml').read_text(encoding='utf-8-sig')).get('scene', {}).get('kind', '2d') == '3d'
                          for s in scenes if (reference_scenes/f'{s}.scene.toml').is_file())
        project = stage_project(args.language, scenes, build, ROOT/'.temp/gameplay-parity', fixtures_3d)
        report.update(project=str(project), artifacts=str(directory),
                      sceneSha256={s: hashlib.sha256((project/'scenes'/f'{s}.scene.toml').read_bytes()).hexdigest() for s in scenes})
        deadline = started+args.timeout
        if any(scene_kind(project, s) == '3d' for s in scenes):
            report['nativeBake'] = bake_native(project, directory, deadline, _OwnedProcess)
        native_ticks = native_checkpoint_ticks(ticks)
        shifted = lambda events: [dict(e, tick=e['tick']+NATIVE_WARMUP_UPDATES) for e in events]
        request = dict(project=str(project), operation='test', target='desktop', ticks=native_ticks[-1], step=1/60, scene=scenes[0],
                       timeoutMs=min(600000, args.timeout*1000), launchConfig=native_launch(shifted(inputs[scenes[0]]), native_ticks),
                       cases=[dict(name=s, scene=s, launchConfig=native_launch(shifted(inputs[s]), native_ticks)) for s in scenes])
        job = start_job(request, repo=ROOT)
        while job['report']['status'] not in TERMINAL:
            if time.monotonic() >= deadline:
                raise TimeoutError('Shared native/browser qualification deadline exceeded')
            time.sleep(.05)
            job = status_job(job['jobId'], repo=ROOT)
        report['nativeJob'] = job
        if job['report']['status'] != 'passed':
            raise RuntimeError('Native gameplay run failed; inspect nativeJob report and runtime logs')
        native_dir = Path(job['reportPath']).parent
        native_states, native_map = {}, {}
        for i, s in enumerate(scenes):
            raw = {t: json.loads((native_dir/'cases'/str(i)/f'checkpoint-{t}.json').read_text(encoding='utf-8')) for t in native_ticks}
            native_states[s], native_map[s] = select_native(raw, ticks)
        browser_request = directory/'browser-request.json'
        browser_request.write_text(json.dumps(dict(
            root=str(web_root), language=args.language, playwright=args.playwright, declaredScenes=sorted(p.name[:-len('.scene.toml')] for p in (project/'scenes').glob('*.scene.toml')),
            deadlineMs=max(30000, int((deadline-time.monotonic())*1000)),
            scenes=[dict(scene=s, checkpoints=ticks, input=[dict(tick=e['tick'], key=key_code(e['key']), down=e['down']) for e in inputs[s]]) for s in scenes])),
            encoding='utf-8')
        browser_report = directory/'browser.json'
        with (directory/'browser.log').open('wb') as stream:
            worker = _OwnedProcess([shutil.which('node') or 'node', str(Path(__file__).with_suffix('.mjs')), '--request='+str(browser_request), '--report='+str(browser_report)], ROOT, stream)
            try:
                while worker.poll() is None:
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Shared native/browser qualification deadline exceeded')
                    time.sleep(.03)
                if worker.poll() != 0:
                    if browser_report.is_file():
                        report['browser'] = json.loads(browser_report.read_text(encoding='utf-8'))
                    raise RuntimeError('Browser run failed; inspect '+str(browser_report))
            finally:
                worker.close()
        report['browser'] = json.loads(browser_report.read_text(encoding='utf-8'))
        web = {item['scene']: {int(t): state for t, state in item['checkpoints'].items()} for item in report['browser']['scenes']}
        for scene in scenes:
            case = compare_case(scene, ticks, native_states[scene], web.get(scene, {}), tolerance)
            case['nativeCheckpointForSimulatedTick'] = native_map[scene]
            # Sensitivity control: a drift of twice the tolerance (at least 1e-3) must be detected.
            drift = max(2*tolerance, 1e-3)
            control = compare_case(scene, ticks, native_states[scene], perturb(web.get(scene, {}), drift), tolerance)
            case['perturbationControl'] = dict(drift=drift, detected=control['status'] == 'failed',
                                               mismatches=sum(len(c['mismatches']) for c in control['checkpoints']))
            if not case['perturbationControl']['detected']:
                case['status'] = 'failed'
            report['cases'].append(case)
        report['status'] = 'passed' if report['cases'] and all(c['status'] == 'passed' for c in report['cases']) else 'failed'
    except (Exception, KeyboardInterrupt) as error:
        report['diagnostics'].append(dict(code='GAMEPLAY_PARITY', message=str(error)))
    finally:
        if job and job['report']['status'] not in TERMINAL:
            cancel_job(job['jobId'], repo=ROOT)
            cleanup_deadline = time.monotonic()+15
            while job['report']['status'] not in TERMINAL and time.monotonic() < cleanup_deadline:
                time.sleep(.05)
                job = status_job(job['jobId'], repo=ROOT)
            report['nativeCancellationCompleted'] = job['report']['status'] in TERMINAL
            if not report['nativeCancellationCompleted']:
                report['diagnostics'].append(dict(code='GAMEPLAY_NATIVE_CLEANUP', message='Native supervisor has not acknowledged process retirement; inspect nativeJob'))
        report['durationMs'] = int((time.monotonic()-started)*1000)
        args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    for case in report['cases']:
        for checkpoint in case['checkpoints']:
            for mismatch in checkpoint['mismatches']:
                print('MISMATCH', json.dumps(mismatch), file=sys.stderr)
    print(json.dumps(dict(status=report['status'], report=str(args.report))))
    return 0 if report['status'] == 'passed' else 1


def bake_native(project, directory, deadline, owned_process):
    packer = ROOT/('build/default/tools/RelWithDebInfo/AssetPacker.exe' if os.name == 'nt' else 'build/linux-clang/tools/AssetPacker')
    if not packer.is_file():
        raise RuntimeError('Build the native AssetPacker before 3D gameplay parity: '+str(packer))
    log = directory/'native-bake.log'
    with log.open('wb') as stream:
        worker = owned_process([str(packer), 'bake-all', str(project)], ROOT, stream)
        try:
            while worker.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Native model bake exceeded the shared deadline')
                time.sleep(.03)
            if worker.poll() != 0:
                raise RuntimeError('Native model bake failed; inspect '+str(log))
        finally:
            worker.close()
    return dict(status='passed', log=str(log))


if __name__ == '__main__':
    raise SystemExit(main())
