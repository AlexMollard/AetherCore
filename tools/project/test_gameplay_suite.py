"""Real build-once suite qualification; opt in with AETHER_DESKTOP_REFERENCE_TESTS=1."""
import copy
import json
import os
from pathlib import Path
import time
import unittest
import uuid

from benchmark_test_suite import cases, execute
from compare_reference import prepare
from gameplay_jobs import validate_report
from reference_fixture import ROOT


@unittest.skipUnless(os.environ.get('AETHER_DESKTOP_REFERENCE_TESTS') == '1', 'requires native SDK, both gameplay toolchains and a GPU')
class GameplaySuiteTests(unittest.TestCase):
    def test_both_languages_compile_once_and_report_each_fresh_case(self):
        root = ROOT / 'logs/agent-test-suite/qualification' / str(uuid.uuid4())
        for language in ('Rust', 'CSharp'):
            with self.subTest(language=language):
                project = ROOT / '.temp/agent-test-suite' / root.name / language
                prepare(project, language)
                authored = cases()
                for item, expected in zip(authored, (10, 0, 5)):
                    item['launchConfig']['checkpoints'] = [dict(tick=10, assertions=[
                        dict(entity='Player', type='ReferencePlayer', property='ticks', equals=10),
                        dict(entity='Player', type='ReferencePlayer', property='score', equals=expected)])]
                request = dict(project=str(project), operation='test', cases=authored)
                wrong = copy.deepcopy(request)
                wrong['cases'][1]['launchConfig']['assertions'][0]['equals'] = 999
                failed, _ = execute(wrong, root / language / 'failed', time.monotonic() + 90)
                self.assertEqual(failed['status'], 'failed', failed)
                self.assertEqual([item['status'] for item in failed['requiredChecks'] if item['name'].startswith('case:')], ['passed', 'failed', 'passed'])
                diagnostic = next(item for item in failed['diagnostics'] if item['code'] == 'GAMEPLAY_ASSERTION_VALUE_MISMATCH')
                self.assertEqual(json.loads(diagnostic['message'])['case'], 'no-input')
                self.assertTrue(Path(diagnostic['source']['file']).is_file())

                passed, _ = execute(request, root / language / 'repair', time.monotonic() + 90)
                self.assertEqual(passed['status'], 'passed', passed)
                compilation = 'cargo-compilation' if language == 'Rust' else 'managed-compilation'
                self.assertEqual(sum(check['name'] == compilation for check in passed['requiredChecks']), 1)
                modules = []
                for index in range(3):
                    folder = root / language / 'repair/cases' / str(index)
                    child = json.loads((folder / 'report.json').read_text(encoding='utf-8'))
                    validate_report(child, passed['jobId'] + ':' + str(index), 0)
                    checkpoint = json.loads((folder / 'checkpoint-10.json').read_text(encoding='utf-8'))
                    self.assertTrue(all(item['passed'] for item in checkpoint['assertions']))
                    launch = json.loads((folder / 'request.json').read_text(encoding='utf-8'))
                    modules.append(launch.get('module') or launch['managedDir'])
                    self.assertNotIn('cases', launch)
                    self.assertFalse((folder / 'build').exists())
                self.assertEqual(len(set(modules)), 1)
                summary = json.loads((root / language / 'repair/suite-summary.json').read_text(encoding='utf-8'))
                self.assertEqual(summary['status'], 'passed')
                self.assertEqual([item['name'] for item in summary['cases']], [item['name'] for item in authored])

                invalid = copy.deepcopy(request)
                invalid['cases'][-1]['launchConfig']['input'] = [dict(tick=-1, key='space', down=True)]
                rejected, _ = execute(invalid, root / language / 'invalid', time.monotonic() + 30)
                self.assertEqual(rejected['phase'], 'preflight', rejected)
                self.assertEqual(rejected['diagnostics'][0]['code'], 'GAMEPLAY_CONFIG')
                self.assertFalse((root / language / 'invalid/build').exists())
                self.assertFalse((root / language / 'invalid/cases').exists())

                source = project / ('scripts/src/lib.rs' if language == 'Rust' else 'scripts/Reference.cs')
                original = source.read_bytes()
                try:
                    source.write_bytes(original + (b'\ncompile_error!("suite compile failure");\n' if language == 'Rust' else b'\n#error suite compile failure\n'))
                    broken, _ = execute(request, root / language / 'compile-failure', time.monotonic() + 60)
                    self.assertEqual(broken['status'], 'failed')
                    self.assertEqual(broken['phase'], 'build')
                    self.assertFalse((root / language / 'compile-failure/cases').exists())
                finally:
                    source.write_bytes(original)

    def test_timeout_stops_remaining_cases_and_keeps_partial_summary(self):
        root = ROOT / 'logs/agent-test-suite/deadline' / str(uuid.uuid4())
        for language in ('Rust', 'CSharp'):
            with self.subTest(language=language):
                request = dict(project=str(ROOT / 'projects' / ('AgentPipeline' + language)), operation='test', timeoutMs=8000, cases=cases()[:2])
                request['cases'][0]['ticks'] = 1000000
                report, elapsed = execute(request, root / language, time.monotonic() + 30)
                self.assertEqual(report['status'], 'failed', report)
                self.assertTrue(any(item['code'] == 'GAMEPLAY_TIMEOUT' for item in report['diagnostics']), report)
                self.assertLess(elapsed, 20000)
                summary = json.loads((root / language / 'suite-summary.json').read_text(encoding='utf-8'))
                self.assertEqual(summary['cases'][1]['status'], 'skipped')
                self.assertFalse((root / language / 'cases/1/request.json').exists())


if __name__ == '__main__':
    unittest.main()
