"""Static MCP gameplay jobs work without a live editor endpoint."""

import importlib.util
import json
from pathlib import Path
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('gameplay_jobs_mcp', Path(__file__).with_name('aethercore_mcp.py'))
mcp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mcp)


class GameplayJobTransportTests(unittest.TestCase):
    def test_published_job_schema_can_describe_the_reference_verification(self):
        fixture = Path(__file__).resolve().parents[2] / 'projects/AgentPipelineCSharp/reference-test.json'
        launch = json.loads(fixture.read_text(encoding='utf-8'))
        properties = mcp.GAMEPLAY_JOB_START['inputSchema']['properties']['launchConfig']['properties']
        self.assertTrue(set(launch).issubset(properties), set(launch) - set(properties))
        self.assertIn(launch['profile'], properties['profile']['enum'])
        checkpoint = properties['checkpoints']['items']
        self.assertTrue({'tick', 'assertions'}.issubset(checkpoint['required']))
        self.assertEqual(checkpoint['properties']['assertions']['items'], properties['assertions']['items'])

    def test_job_tools_remain_listed_without_an_editor(self):
        with mock.patch.object(mcp, '_engine_tools', return_value=[]):
            names = {tool['name'] for tool in mcp._list_tools()}
        self.assertTrue({'gameplay_job_start', 'gameplay_job_status', 'gameplay_job_cancel'}.issubset(names))

    def test_start_preserves_request_and_structured_queued_content(self):
        request = dict(project='世界 & project', operation='test', timeoutMs=1000,
                       launchConfig=dict(readSaves=False, input=[dict(tick=1, key='Space', down=True)]))
        result = dict(jobId='uuid', report=dict(status='queued', exitCode=None))
        with mock.patch.object(mcp.gameplay_jobs, 'start_job', return_value=result) as start, \
                mock.patch.object(mcp, '_engine_tools', side_effect=AssertionError('editor must not be queried')):
            response = mcp._handle(dict(id=1, method='tools/call', params=dict(name='gameplay_job_start', arguments=request)))
        self.assertEqual(start.call_args.args, (request,))
        self.assertFalse(response['result']['isError'])
        self.assertEqual(response['result']['structuredContent'], result)
        self.assertEqual(json.loads(response['result']['content'][0]['text']), result)

    def test_status_and_cancel_keep_terminal_failure_report(self):
        result = dict(jobId='uuid', report=dict(status='cancelled', exitCode=130))
        for action in ('status', 'cancel'):
            with self.subTest(action=action), mock.patch.object(mcp.gameplay_jobs, action + '_job', return_value=result), \
                    mock.patch.object(mcp, '_engine_tools', side_effect=AssertionError('editor must not be queried')):
                response = mcp._handle(dict(id=2, method='tools/call', params=dict(name='gameplay_job_' + action, arguments={'jobId': 'uuid'})))
                self.assertTrue(response['result']['isError'])
                self.assertEqual(response['result']['structuredContent'], result)


if __name__ == '__main__':
    unittest.main()
