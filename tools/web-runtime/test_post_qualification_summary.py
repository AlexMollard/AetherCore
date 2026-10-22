import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('post_summary', Path(__file__).with_name('post-qualification-summary.py'))
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class QualificationSummaryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value), encoding='utf8')
        return path

    def report(self, name='baseline', features=None):
        cases = []
        for feature in features or summary.post.FEATURES:
            scene, overrides = summary.post.FEATURES[feature]
            item = dict(feature=feature, scene=scene, status='passed',
                        onGraphics=summary.post.parity.graphics_settings(overrides),
                        offGraphics=summary.post.parity.graphics_settings({}),
                        backends={backend: dict(passed=True, checks=dict(semantic=True)) for backend in ('native', 'web')})
            for key, graphics in [('enabledReport', item['onGraphics']), ('controlReport', item['offGraphics'])]:
                child = dict(schemaVersion=1, operation='lighting-parity', status='passed', diagnostics=[],
                             scope=summary.CHILD_SCOPE, requestedScenes=[scene], requestedOutputExtent=[512, 512],
                             nativeCaptureTick=10, browserCaptureTick=10, graphics=graphics,
                             sceneSha256={scene: summary.scene_hash(scene)}, browserPackageSha256='a'*64,
                             sharedFixtureTreeSha256='b'*64,
                             cases=[dict(scene=scene, status='passed', width=512, height=512,
                                         comparison=dict(passed=True), nativeFeatures=dict(passed=True,checks=dict(visible=True)),
                                         webFeatures=dict(passed=True,checks=dict(visible=True)), frameEvidenceValidation=dict(passed=True))])
                item[key] = str(self.write(f'{name}-{feature}-{key}.json', child))
            cases.append(item)
        return dict(schemaVersion=1, operation='post-parity', status='passed', scope=summary.SCOPE,
                    diagnostics=[], requestedFeatures=[item['feature'] for item in cases], cases=cases)

    def test_incremental_fix_preserves_original_failed_status_and_failure_provenance(self):
        baseline = self.report()
        baseline['status'] = 'failed'
        baseline['cases'][0]['status'] = 'failed'
        baseline['cases'][0]['backends']['native']['passed'] = False
        path = self.write('baseline.json', baseline)
        original = path.read_bytes()
        correction = self.write('fixed.json', self.report('fixed', [baseline['cases'][0]['feature']]))
        result = summary.combine([path, correction])
        self.assertEqual(result['status'], 'passed')
        self.assertEqual(len(result['cases']), len(summary.post.FEATURES))
        self.assertEqual(result['sources'][0]['status'], 'failed')
        self.assertEqual(len(result['supersededFailures']), 1)
        self.assertEqual(result['supersededFailures'][0]['feature'], baseline['cases'][0]['feature'])
        self.assertEqual(path.read_bytes(), original)

    def test_missing_feature_fails_even_if_every_supplied_case_passes(self):
        data = self.report(features=list(summary.post.FEATURES)[:-1])
        self.assertEqual(summary.combine([self.write('missing.json', data)])['status'], 'failed')

    def test_latest_failure_cannot_be_hidden_by_older_passing_case(self):
        baseline = self.write('baseline.json', self.report())
        bad = self.report('new', ['contrast'])
        bad['status'] = bad['cases'][0]['status'] = 'failed'
        self.assertEqual(summary.combine([baseline, self.write('bad.json', bad)])['status'], 'failed')

    def test_global_diagnostic_is_not_superseded_by_successful_case(self):
        bad = self.report('bad', ['contrast'])
        bad['status'] = 'failed'
        bad['diagnostics'] = [dict(code='POST_PARITY', message='interrupted')]
        result = summary.combine([self.write('bad.json', bad), self.write('good.json', self.report())])
        self.assertEqual(result['status'], 'failed')
        self.assertTrue(any('diagnostics' in item['message'] for item in result['diagnostics']))

    def test_wrong_graphics_duplicate_false_backend_or_scope_are_rejected(self):
        for mutation in ('graphics', 'duplicate', 'backend', 'scope'):
            data = self.report(mutation)
            if mutation == 'graphics':
                data['cases'][0]['onGraphics']['renderScale'] = .5
            elif mutation == 'duplicate':
                data['cases'].append(copy.deepcopy(data['cases'][0]))
            elif mutation == 'backend':
                data['cases'][0]['backends']['web']['checks']['semantic'] = False
            else:
                data['scope'] = 'external publishing qualified'
            with self.subTest(mutation=mutation):
                self.assertEqual(summary.combine([self.write(mutation+'.json', data)])['status'], 'failed')

    def test_child_extent_config_or_scene_hash_mismatch_is_rejected(self):
        for mutation in ('extent', 'config', 'scene'):
            data = self.report(mutation)
            child_path = Path(data['cases'][0]['enabledReport'])
            child = json.loads(child_path.read_text())
            if mutation == 'extent':
                child['requestedOutputExtent'] = [64, 64]
                child['cases'][0].update(width=64, height=64)
            elif mutation == 'config':
                child['graphics']['renderScale'] = .5
            else:
                child['sceneSha256'][data['cases'][0]['scene']] = '0'*64
            child_path.write_text(json.dumps(child), encoding='utf8')
            with self.subTest(mutation=mutation):
                self.assertEqual(summary.combine([self.write(mutation+'.json', data)])['status'], 'failed')

    def test_malformed_backend_and_child_case_are_failures_not_exceptions(self):
        for mutation in ('backend', 'child'):
            data = self.report(mutation)
            if mutation == 'backend':
                data['cases'][0]['backends'] = None
            else:
                child_path = Path(data['cases'][0]['enabledReport'])
                child = json.loads(child_path.read_text())
                child['cases'] = [None]
                child_path.write_text(json.dumps(child), encoding='utf8')
            with self.subTest(mutation=mutation):
                self.assertEqual(summary.combine([self.write(mutation+'.json', data)])['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
