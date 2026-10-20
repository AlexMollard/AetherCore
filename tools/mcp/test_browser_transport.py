import importlib.util
from pathlib import Path
import unittest
spec=importlib.util.spec_from_file_location('browser_mcp',Path(__file__).with_name('aethercore_mcp.py'))
mcp=importlib.util.module_from_spec(spec);spec.loader.exec_module(mcp)
class BrowserTransportTests(unittest.TestCase):
 def test_tool_available_without_running_editor(self):
  self.assertTrue(hasattr(mcp,'RUN_BROWSER_GAUNTLET'),'browser gauntlet MCP tool is missing')
  self.assertEqual(mcp.RUN_BROWSER_GAUNTLET['name'],'run_browser_gauntlet')
 def test_real_unsupported_project_is_structured_failure(self):
  response=mcp._handle(dict(id=1,method='tools/call',params=dict(name='run_browser_gauntlet',arguments=dict(project='projects/AgentPipelineCSharp',skipBuild=True,reportPath='logs/task11/mcp-blocked.json'))))['result']
  self.assertTrue(response['isError'])
  self.assertEqual(response['structuredContent']['status'],'blocked')
  self.assertEqual(response['structuredContent']['target'],'web')
if __name__=='__main__':unittest.main()
