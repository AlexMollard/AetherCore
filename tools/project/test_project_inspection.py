"""Read-only native/MCP qualification; AETHER_PROJECT_INSPECTION_TESTS=1."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest
import uuid

from compare_reference import ROOT, prepare
from gameplay_jobs import validate_report


@unittest.skipUnless(os.environ.get('AETHER_PROJECT_INSPECTION_TESTS') == '1', 'requires the built aether-project worker')
class ProjectInspectionTests(unittest.TestCase):
    def test_fresh_mcp_inspection_in_both_languages_without_toolchains(self):
        directory = ROOT / '.temp/project-inspection' / str(uuid.uuid4())
        environment = dict(os.environ, PYTHONIOENCODING='cp1252')
        for name in ('CMAKE', 'CARGO', 'RUSTC', 'DOTNET'):
            environment['AETHERCORE_' + name] = str(directory / 'missing-tool')

        def call(name, arguments):
            request = dict(jsonrpc='2.0', id=1, method='tools/call', params=dict(name=name, arguments=arguments))
            response = subprocess.run([sys.executable, str(ROOT / 'tools/mcp/aethercore_mcp.py')],
                input=(json.dumps(request, ensure_ascii=False) + '\n').encode('utf-8'), capture_output=True,
                timeout=15, env=environment, creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            self.assertEqual(response.returncode, 0, response.stderr)
            payload = json.loads(response.stdout.decode('utf-8'))['result']
            self.assertIn('structuredContent', payload)
            return payload['structuredContent']

        records = []
        for language in ('Rust', 'CSharp'):
            with self.subTest(language=language):
                project = directory / (language + ' caf\u00e9 & \u4e16\u754c')
                prepare(project, language)
                descriptor = project / 'ProjectSettings.toml'
                text = descriptor.read_text(encoding='utf-8')
                (project / 'scripts').rename(project / 'custom scripts')
                (project / 'scenes').rename(project / 'custom scenes')
                text = text.replace('scripts = "scripts"', 'scripts = "custom scripts"').replace('scenes = "scenes"', 'scenes = "custom scenes"')
                if language == 'CSharp':
                    # Legacy projects continue to default to C#.
                    text = text.replace('[scripting]\nlanguage = "csharp"', '')
                descriptor.write_text(text, encoding='utf-8')
                before = {str(p.relative_to(project)): p.read_bytes() for p in project.rglob('*') if p.is_file()}

                def inspect(scene):
                    start = call('gameplay_job_start', dict(project=str(project), operation='inspect', scene=scene, timeoutMs=10000))
                    deadline = time.monotonic() + 15
                    while True:
                        result = call('gameplay_job_status', dict(jobId=start['jobId']))
                        if result['report']['status'] in ('passed', 'failed', 'cancelled'):
                            break
                        if time.monotonic() >= deadline:
                            call('gameplay_job_cancel', dict(jobId=start['jobId']))
                            self.fail('inspection timed out')
                        time.sleep(.05)
                    validate_report(result['report'], start['jobId'], result['report']['exitCode'])
                    return result

                result = inspect('ReferenceStart')
                self.assertEqual(result['report']['status'], 'passed', result)
                inventory = result['inspection']
                self.assertEqual(inventory['language'], language.lower())
                self.assertFalse(inventory['runtimeVerified'])
                self.assertFalse(inventory['compilerInvoked'])
                self.assertEqual(Path(inventory['paths']['scripts']), project / 'custom scripts')
                self.assertTrue(any(script['type'] == 'ReferencePlayer' for entity in inventory['scene']['entities'] for script in entity['scripts']))
                self.assertEqual(before, {str(p.relative_to(project)): p.read_bytes() for p in project.rglob('*') if p.is_file()})
                artifacts = Path(result['reportPath']).parent
                self.assertFalse(any((artifacts / name).exists() for name in ('build', 'build.log', 'runtime.log', 'ready.json')))
                build_entry = Path(inventory['gameplay']['buildEntry']['path'])
                build_entry.unlink()
                failed = inspect('ReferenceStart')
                self.assertEqual(failed['report']['status'], 'failed')
                self.assertFalse(failed['inspection']['gameplay']['buildEntry']['exists'])
                self.assertTrue(any(item['code'] == 'GAMEPLAY_INSPECT_BUILD_ENTRY' and Path(item['source']['file']) == build_entry for item in failed['report']['diagnostics']))
                records.append(dict(language=language, jobId=result['jobId'], durationMs=result['report']['durationMs'],
                                    customPaths=True, unicode=True, sourceUnchanged=True, missingEntryDiagnosed=True))
        output = ROOT / 'logs/project-inspection-acceptance.json'
        output.write_text(json.dumps(dict(status='passed', checks=records), indent=2), encoding='utf-8')
