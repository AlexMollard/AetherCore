"""Prove visual gates reject regressions using existing captures, without a GPU.

This qualifies the detection gates, not the source runs or renderer. Source-run
failures remain recorded; no threshold or production capture is modified.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


parity = load_module('negative_lighting_parity', 'lighting-parity-checks.py')
post = load_module('negative_post_parity', 'post-parity-checks.py')
ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--material', type=Path, default=ROOT/'logs/task4/material-first-parity.json')
    parser.add_argument('--graph', type=Path, default=ROOT/'logs/task4/graph-third-parity.json')
    parser.add_argument('--post', type=Path, default=ROOT/'logs/task4/post-first.json')
    parser.add_argument('--report', type=Path, default=ROOT/'logs/task4/negative-visual-qualification.json')
    args = parser.parse_args()
    report = dict(schemaVersion=1, operation='offline-negative-visual-qualification', status='failed',
                  scope='Gate sensitivity using captured pixels; does not qualify failed source runs or rerun rendering',
                  cases=[], inputs={}, sourceReports=[], diagnostics=[])

    def read(path):
        path = path.resolve()
        if path.stat().st_size > 32*1024*1024:
            raise ValueError('Negative qualification input exceeds 32 MiB: '+str(path))
        data = path.read_bytes()
        report['inputs'][str(path)] = dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))
        return data

    def source(path):
        data = json.loads(read(path))
        report['sourceReports'].append(dict(path=str(path.resolve()), status=data.get('status')))
        return data

    def capture(data, scene, backend):
        case = next(item for item in data['cases'] if item['scene'] == scene)
        w, h = case['width'], case['height']
        if not (1 <= w <= 2048 and 1 <= h <= 2048):
            raise ValueError('Negative qualification supports captured extents 1..2048')
        path = Path(data['artifacts'])/(scene+('-native.png' if backend == 'native' else '-web.rgba'))
        raw = read(path)
        if backend == 'native':
            actual_w, actual_h, pixels = parity.png(path)
            if (actual_w, actual_h) != (w, h):
                raise ValueError('Native capture extent disagrees with its report')
        else:
            pixels = raw
        if len(pixels) != w*h*4:
            raise ValueError('Captured pixel payload disagrees with its report')
        return w, h, pixels

    def rejected(name, baseline, result, mutation, **extra):
        failed = [key for key, value in result.get('checks', {}).items() if not value]
        if 'tolerances' in result:
            failed = [key for key, limit in result['tolerances'].items() if result[key] > limit]
        passed = baseline['passed'] and not result['passed'] and bool(failed)
        report['cases'].append(dict(name=name, status='passed' if passed else 'failed', mutation=mutation,
                                    baseline=baseline, mutated=result, failedCriteria=failed, **extra))

    try:
        report['oracleSources'] = {}
        for name in ('lighting-parity-checks.py', 'post-parity-checks.py', 'material_effect_oracles.py', 'material-effect-oracles.json'):
            path = Path(__file__).with_name(name)
            report['oracleSources'][name] = hashlib.sha256(read(path)).hexdigest()
        material, graph, post_report = source(args.material), source(args.graph), source(args.post)
        # A broad authored material and a red custom graph exercise different color
        # distributions. Alpha stays intact so these prove RGB gates, not payload checks.
        for data, scene in ((material, 'BrowserWater'), (graph, 'BrowserGraph')):
            w, h, native = capture(data, scene, 'native')
            _, _, web = capture(data, scene, 'web')
            baseline = parity.compare(native, web, w, h)
            black = bytearray(web)
            black[0::4] = black[1::4] = black[2::4] = bytes(w*h)
            rejected(scene+' black frame', baseline, parity.compare(native, black, w, h),
                     'Replace captured Web RGB with zero; preserve alpha')
            semantic = parity.feature_checks(scene, web, w, h)
            rejected(scene+' black semantic', semantic, parity.feature_checks(scene, black, w, h),
                     'Replace captured Web RGB with zero; preserve alpha')
            if scene == 'BrowserGraph':
                wrong = bytearray(web)
                wrong[0::4], wrong[2::4] = web[2::4], web[0::4]
                rejected(scene+' swapped red/blue', baseline, parity.compare(native, wrong, w, h),
                         'Swap captured Web red and blue channels; preserve green/alpha')
        # Compare each actual backend against its captured control. Substituting OFF
        # for ON must fail the feature-specific gate even when both backends agree.
        for feature in ('saturation', 'vignette', 'bloom'):
            pair = next(item for item in post_report['cases'] if item['feature'] == feature)
            enabled, control = source(Path(pair['enabledReport'])), source(Path(pair['controlReport']))
            for backend in ('native', 'web'):
                w, h, on = capture(enabled, pair['scene'], backend)
                ow, oh, off = capture(control, pair['scene'], backend)
                if (w, h) != (ow, oh):
                    raise ValueError('Enabled/control extents differ')
                baseline = post.effect_checks(feature, on, off, w, h)
                rejected(feature+' disabled '+backend, baseline, post.effect_checks(feature, off, off, w, h),
                         'Substitute actual captured OFF pixels for enabled pixels', backend=backend)
        pair = next(item for item in parity.MATERIAL_PAIRS if item['name'] == 'water shading')
        for backend in ('native', 'web'):
            w, h, on = capture(material, pair['on'], backend)
            ow, oh, off = capture(material, pair['off'], backend)
            if (w, h) != (ow, oh):
                raise ValueError('Material/control extents differ')
            baseline = parity.material_pair_features(pair, on, off, w, h)
            rejected('water disabled '+backend, baseline, parity.material_pair_features(pair, off, off, w, h),
                     'Substitute actual captured material OFF pixels for enabled pixels', backend=backend)
        report['status'] = 'passed' if report['cases'] and all(item['status'] == 'passed' for item in report['cases']) else 'failed'
    except Exception as error:
        report['diagnostics'].append(dict(code='NEGATIVE_VISUAL_INPUT', message=str(error)))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(status=report['status'], cases=len(report['cases']), report=str(args.report))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
