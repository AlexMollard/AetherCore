"""Owned editor tools route through a receipt, independently of the default port."""

import importlib.util
import json
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('agent_editor_mcp', Path(__file__).with_name('aethercore_mcp.py'))
mcp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mcp)


class AgentEditorTransportTests(unittest.TestCase):
    def call(self, name, arguments, stdout, code=0):
        def helper(argv, **kwargs):
            kwargs['stdout'].write(json.dumps(stdout).encode('utf-8'))
            return subprocess.CompletedProcess(argv, code)
        with mock.patch.object(mcp.subprocess, 'run', side_effect=helper) as run, \
                mock.patch.object(mcp, '_engine_tools', side_effect=AssertionError('default endpoint must not be queried')):
            result = mcp._handle(dict(id=1, method='tools/call', params=dict(name=name, arguments=arguments)))
        return result['result'], run.call_args

    def test_static_tools_are_available_without_an_editor(self):
        with mock.patch.object(mcp, '_engine_tools', return_value=[]):
            names = {tool['name'] for tool in mcp._list_tools()}
        self.assertTrue({'agent_editor_launch', 'agent_editor_call', 'agent_editor_close'}.issubset(names))

    def test_launch_defaults_to_background_and_preserves_unicode_identity(self):
        receipt = dict(receiptPath='logs/launch.json', pid=123, name='Pog Champ café & 世界', port=8891)
        result, call = self.call('agent_editor_launch', dict(project='project & 世界', name=receipt['name'], port=8891), receipt)
        self.assertFalse(result['isError'])
        self.assertEqual(result['structuredContent']['receiptPath'], receipt['receiptPath'])
        self.assertIn('cleanupInstructions', result['structuredContent'])
        argv = call.args[0]
        self.assertEqual(argv[argv.index('-Name') + 1], receipt['name'])
        self.assertNotIn('-Visible', argv)
        self.assertEqual(call.kwargs['encoding'], 'utf-8')

    def test_visible_is_an_explicit_opt_in(self):
        _, call = self.call('agent_editor_launch', dict(project='p', name='agent', port=8891, background=False), {'receiptPath': 'launch.json'})
        self.assertIn('-Visible', call.args[0])

    def test_call_uses_receipt_and_utf8_json_file_without_global_port(self):
        parameters = dict(name='世界\u0000 café', nested={'symbols': '& | $(literal)'})
        def run(argv, **kwargs):
            self.assertNotIn('--port', argv)
            self.assertEqual(argv[argv.index('-Receipt') + 1], 'owned & receipt.json')
            self.assertEqual(argv[argv.index('-Method') + 1], 'scene_create')
            self.assertEqual(json.loads(Path(argv[argv.index('-ParamsPath') + 1]).read_text(encoding='utf-8')), parameters)
            kwargs['stdout'].write(json.dumps({'ok': True, 'result': {'id': 77}}).encode('utf-8'))
            return subprocess.CompletedProcess(argv, 0)
        with mock.patch.object(mcp.subprocess, 'run', side_effect=run), \
                mock.patch.object(mcp, '_engine_tools', side_effect=AssertionError('wrong editor endpoint')):
            result = mcp._call_tool('agent_editor_call', dict(receipt='owned & receipt.json', method='scene_create', arguments=parameters))
        self.assertEqual(result['result']['result'], {'id': 77})

    def test_structured_endpoint_failure_is_retained(self):
        payload = dict(ok=False, message='prerequisite failed', result=dict(ok=False, report=dict(schemaVersion=1, status='failed', exitCode=1)))
        result, _ = self.call('agent_editor_call', dict(receipt='launch.json', method='engine_play'), payload, code=1)
        self.assertTrue(result['isError'])
        self.assertEqual(result['structuredContent']['result']['report']['status'], 'failed')

    def test_close_requires_explicit_discard_and_keeps_receipt_routing(self):
        for discard in (False, True):
            with self.subTest(discard=discard):
                result, call = self.call('agent_editor_close', dict(receipt='owned.json', discardChanges=discard), {'closed': True, 'cleanupRequired': False})
                self.assertFalse(result['isError'])
                argv = call.args[0]
                self.assertEqual(argv[argv.index('-Receipt') + 1], 'owned.json')
                self.assertEqual('-DiscardChanges' in argv, discard)


@unittest.skipUnless(shutil.which('pwsh'), 'PowerShell is required for the helper boundary test')
class AgentEditorInvokeScriptTests(unittest.TestCase):
    def invoke(self, *, reject=False, failure=False):
        with tempfile.TemporaryDirectory(prefix='owned editor café & ') as temporary:
            root = Path(temporary)
            shutil.copyfile(mcp.REPO / 'scripts/Invoke-AgentEditor.ps1', root / 'Invoke-AgentEditor.ps1')
            (root / 'AgentEditor.Common.ps1').write_text(r'''
function Get-AgentEditorProcess($Receipt) { return @{ Id = 123 } }
function Assert-AgentEditorIdentity($Receipt, $Info) {
    if ($Receipt.rejectIdentity) { throw 'Pog Champ, ownership mismatch.' }
}
function Invoke-AgentEditorControl($Receipt, [string]$Method, $Parameters = @{}, [int]$TimeoutMs = 5000, [switch]$AllowStructuredFailure) {
    Add-Content -LiteralPath $Receipt.tracePath -Value $Method
    if ($Method -eq 'info') { return @{ pid = 123 } }
    if ($Method -eq 'describe') { return @{ methods = @(@{ name = 'scene.create'; tool = 'scene_create' }) } }
    if ($Receipt.failCall) { return [pscustomobject]@{ ok = $false; report = @{ schemaVersion = 1; status = 'failed'; exitCode = 1 } } }
    return @{ receivedMethod = $Method; received = $Parameters; port = $Receipt.port }
}
''', encoding='utf-8')
            receipt = dict(port=8891, rejectIdentity=reject, failCall=failure,
                           tracePath=str(root / 'calls.txt'), cleanupInstructions='Pog Champ, close this owned editor.')
            (root / 'receipt.json').write_text(json.dumps(receipt), encoding='utf-8')
            parameters = dict(name='世界\u0000 café', literal='& | $(hello)')
            (root / 'params.json').write_text(json.dumps(parameters, ensure_ascii=False), encoding='utf-8')
            process = subprocess.run(['pwsh', '-NoProfile', '-File', str(root / 'Invoke-AgentEditor.ps1'),
                                      '-Receipt', str(root / 'receipt.json'), '-Method', 'scene_create',
                                      '-ParamsPath', str(root / 'params.json')], capture_output=True, text=True, encoding='utf-8')
            return process.returncode, json.loads(process.stdout), (root / 'calls.txt').read_text(encoding='utf-8'), parameters

    def test_real_powershell_script_resolves_the_owned_manifest_and_preserves_arguments(self):
        code, payload, calls, parameters = self.invoke()
        self.assertEqual(code, 0)
        self.assertEqual(calls.splitlines(), ['info', 'describe', 'scene.create'])
        self.assertEqual(payload['result']['received'], parameters)
        self.assertEqual(payload['result']['port'], 8891)

    def test_identity_failure_prevents_manifest_lookup_and_requested_method(self):
        code, payload, calls, _ = self.invoke(reject=True)
        self.assertEqual(code, 1)
        self.assertEqual(calls.splitlines(), ['info'])
        self.assertFalse(payload['ok'])
        self.assertTrue(payload['cleanupRequired'])

    def test_real_powershell_script_retains_endpoint_failure_report(self):
        code, payload, _, _ = self.invoke(failure=True)
        self.assertEqual(code, 1)
        self.assertFalse(payload['ok'])
        self.assertEqual(payload['result']['report']['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
