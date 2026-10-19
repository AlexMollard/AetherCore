"""Explicit GPU integration gate; enable with AETHER_DESKTOP_REFERENCE_TESTS=1."""
import json
import os
from pathlib import Path
import unittest
import uuid

from compare_reference import ROOT, project_tool, run


@unittest.skipUnless(os.environ.get("AETHER_DESKTOP_REFERENCE_TESTS") == "1", "requires the built native desktop runtime/GPU")
class DesktopReferenceTests(unittest.TestCase):
    def test_final_scene_detach_failure_cannot_publish_success(self):
        previous = os.environ.get("AETHER_REFERENCE_DETACH_FAILURE")
        os.environ["AETHER_REFERENCE_DETACH_FAILURE"] = "1"
        try:
            for language in ("Rust", "CSharp"):
                with self.subTest(language=language):
                    project = ROOT / "projects" / ("AgentPipeline" + language)
                    request = dict(project=str(project), operation="test", scene="ReferenceStart", ticks=90,
                                   timeoutMs=120000, launchConfig=json.loads((project / "reference-test.json").read_text(encoding="utf-8")))
                    directory = ROOT / "logs/desktop-reference-tests" / str(uuid.uuid4())
                    outcome = run(project_tool(), request, directory, "detach")
                    report = outcome[0]
                    self.assertEqual(report["status"], "failed", str(directory))
                    self.assertTrue(any("detach" in item["message"].lower() for item in report["diagnostics"]), report)
        finally:
            if previous is None:
                os.environ.pop("AETHER_REFERENCE_DETACH_FAILURE", None)
            else:
                os.environ["AETHER_REFERENCE_DETACH_FAILURE"] = previous


if __name__ == "__main__":
    unittest.main()
