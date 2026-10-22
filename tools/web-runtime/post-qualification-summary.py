"""Combine ordered incremental post-effect qualifications without hiding failures.

No capture, build, or original-report mutation occurs. The last case for each
maintained feature wins; earlier failures and every source digest remain visible.
Different binary/package hashes are recorded as separate runs, never one run.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
spec = importlib.util.spec_from_file_location('post_parity_summary_source', Path(__file__).with_name('post-parity-checks.py'))
post = importlib.util.module_from_spec(spec)
spec.loader.exec_module(post)

SCOPE = 'Selected authored post effects, independent on/off backend semantics and unchanged image parity limits; no publishing qualification'
CHILD_SCOPE = 'Explicit authored scene and graphics selection; no unselected effects or external-host qualification'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def scene_hash(scene):
    return digest((post.ROOT / 'resources/web/reference/scenes' / (scene+'.scene.toml')).read_bytes())


def read_report(path):
    raw = path.read_bytes()
    data = json.loads(raw)
    if not isinstance(data, dict):
        raise ValueError('Report must be an object: '+str(path))
    return data, dict(path=str(path), status=data.get('status'), sha256=digest(raw))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def passed_checks(value):
    return (isinstance(value, dict) and value.get('passed') is True
            and isinstance(value.get('checks'), dict) and bool(value['checks'])
            and all(result is True for result in value['checks'].values()))


def backends_passed(item):
    backends = item.get('backends')
    return isinstance(backends, dict) and all(passed_checks(backends.get(backend)) for backend in ('native', 'web'))


def result_passed(value):
    return isinstance(value, dict) and value.get('passed') is True


def combine(paths):
    result = dict(schemaVersion=1, operation='post-qualification-summary', status='failed',
                  scope=SCOPE, expectedFeatures=list(post.FEATURES), sources=[], childSources=[],
                  cases=[], supersededFailures=[], diagnostics=[],
                  ordering='Explicit command-line report order; last case for each feature wins',
                  provenance='Incremental runs may use different binaries and fixture trees; original digests and selected per-case evidence are preserved')
    latest, history, children = {}, [], {}
    extent = None
    seen_paths = set()
    for source in paths:
        path = Path(source).resolve()
        try:
            require(path not in seen_paths, 'Duplicate input report: '+str(path))
            seen_paths.add(path)
            report, provenance = read_report(path)
            result['sources'].append(provenance)
            require(report.get('schemaVersion') == 1 and report.get('operation') == 'post-parity', 'Not a post-parity schema1 report')
            require(report.get('scope') == SCOPE, 'Report scope mismatch')
            require(report.get('status') in ('passed', 'failed'), 'Invalid original report status')
            require(report.get('diagnostics') == [], 'Unexpected global diagnostics in '+str(path))
            cases = report.get('cases')
            require(isinstance(cases, list) and bool(cases), 'No feature cases')
            names = [item.get('feature') for item in cases if isinstance(item, dict)]
            require(len(names) == len(cases) and len(set(names)) == len(names), 'Malformed or duplicate feature cases')
            require(report.get('requestedFeatures') == names, 'Requested feature list does not match cases')
            for item in cases:
                feature = item['feature']
                require(feature in post.FEATURES, 'Unknown feature: '+str(feature))
                scene, overrides = post.FEATURES[feature]
                require(item.get('scene') == scene, 'Feature scene mismatch: '+feature)
                require(item.get('status') in ('passed', 'failed'), 'Invalid case status: '+feature)
                require(item.get('onGraphics') == post.parity.graphics_settings(overrides), 'Feature on graphics mismatch: '+feature)
                require(item.get('offGraphics') == post.parity.graphics_settings({}), 'Feature off graphics mismatch: '+feature)
                entry = dict(item, sourceReport=str(path), sourceSha256=provenance['sha256'])
                captures = {}
                for key, graphics in [('enabledReport', item['onGraphics']), ('controlReport', item['offGraphics'])]:
                    child_path = Path(item.get(key, ''))
                    require(bool(item.get(key)), 'Missing '+key+': '+feature)
                    child_path = (path.parent/child_path).resolve() if not child_path.is_absolute() else child_path.resolve()
                    if child_path not in children:
                        child, child_provenance = read_report(child_path)
                        children[child_path] = (child, child_provenance)
                        result['childSources'].append(child_provenance)
                    child, child_provenance = children[child_path]
                    require(child.get('schemaVersion') == 1 and child.get('operation') == 'lighting-parity', 'Invalid child report schema')
                    require(child.get('scope') == CHILD_SCOPE, 'Child scope mismatch: '+feature)
                    require(child.get('diagnostics') == [], 'Unexpected child global diagnostics: '+feature)
                    if isinstance(child.get('browser'), dict):
                        require(child['browser'].get('diagnostics') == [], 'Unexpected browser global diagnostics: '+feature)
                    job = child.get('nativeJob', {}).get('report', {})
                    if job:
                        require(job.get('diagnostics') == [], 'Unexpected native global diagnostics: '+feature)
                    require(child.get('requestedScenes') == [scene], 'Child scene mismatch: '+feature)
                    require(child.get('graphics') == graphics, 'Child graphics mismatch: '+feature)
                    require(child.get('nativeCaptureTick') == 10 and child.get('browserCaptureTick') == 10, 'Capture tick mismatch: '+feature)
                    size = child.get('requestedOutputExtent')
                    require(isinstance(size, list) and len(size) == 2 and all(type(n) is int and 1 <= n <= 4096 for n in size), 'Invalid capture extent')
                    if extent is None:
                        extent = size
                    require(size == extent, 'Output extent scope mismatch: '+feature)
                    child_cases = child.get('cases')
                    require(isinstance(child_cases, list) and len(child_cases) == 1 and isinstance(child_cases[0], dict) and child_cases[0].get('scene') == scene, 'Malformed child cases: '+feature)
                    require([child_cases[0].get('width'), child_cases[0].get('height')] == size, 'Child case extent mismatch: '+feature)
                    captures[key] = dict(child_provenance, sceneSha256=child.get('sceneSha256', {}).get(scene),
                                         browserPackageSha256=child.get('browserPackageSha256'),
                                         sharedFixtureTreeSha256=child.get('sharedFixtureTreeSha256'))
                entry['captures'] = captures
                history.append(entry)
                latest[feature] = entry
        except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
            result['diagnostics'].append(dict(code='POST_SUMMARY_SOURCE', report=str(path), message=str(error)))
    for feature, (scene, overrides) in post.FEATURES.items():
        if feature not in latest:
            result['diagnostics'].append(dict(code='POST_SUMMARY_MISSING', message='Missing feature: '+feature))
            continue
        item = latest[feature]
        valid = item['status'] == 'passed' and backends_passed(item)
        for capture in item['captures'].values():
            child = children[Path(capture['path'])][0]
            case = child['cases'][0]
            valid = valid and child.get('status') == 'passed' and case.get('status') == 'passed'
            valid = valid and result_passed(case.get('comparison'))
            valid = valid and all(passed_checks(case.get(backend+'Features')) for backend in ('native', 'web'))
            valid = valid and result_passed(case.get('frameEvidenceValidation'))
            valid = valid and capture['sceneSha256'] == scene_hash(scene)
            valid = valid and all(isinstance(capture.get(key), str) and len(capture[key]) == 64 for key in ('browserPackageSha256', 'sharedFixtureTreeSha256'))
        profile = dict(scene=scene, sceneSha256=scene_hash(scene), onGraphics=item['onGraphics'],
                       offGraphics=item['offGraphics'], outputExtent=extent, captureTick=10)
        result['cases'].append(dict(item, qualificationStatus='passed' if valid else 'failed',
                                    profileSha256=digest(json.dumps(profile, sort_keys=True, separators=(',', ':')).encode()), profile=profile))
        if not valid:
            result['diagnostics'].append(dict(code='POST_SUMMARY_CASE', feature=feature, message='Newest case lacks passing backend/image/frame checks or current fixture provenance'))
    for item in history:
        if item is not latest.get(item['feature']) and (item['status'] != 'passed' or not backends_passed(item)):
            result['supersededFailures'].append(dict(feature=item['feature'], sourceReport=item['sourceReport'],
                                                     sourceSha256=item['sourceSha256'], originalCase=item,
                                                     supersededBy=latest[item['feature']]['sourceReport']))
    result['outputExtent'] = extent
    if not result['diagnostics'] and len(result['cases']) == len(post.FEATURES):
        result['status'] = 'passed'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reports', nargs='+', type=Path, required=True, help='Oldest to newest; input reports are never modified')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    output = args.report.resolve()
    if output in {path.resolve() for path in args.reports}:
        parser.error('--report must not overwrite an input report')
    report = combine(args.reports)
    if output in {Path(item['path']) for item in report['childSources']}:
        parser.error('--report must not overwrite a child report')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf8')
    print(json.dumps(dict(status=report['status'], features=len(report['cases']), report=str(output))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
