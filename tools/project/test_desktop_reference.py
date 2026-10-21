"""Explicit GPU integration gate; enable with AETHER_DESKTOP_REFERENCE_TESTS=1."""
import json
import os
from pathlib import Path
import unittest
import uuid

from compare_reference import ROOT, prepare, project_tool, run
from gameplay_jobs import validate_report


@unittest.skipUnless(os.environ.get("AETHER_DESKTOP_REFERENCE_TESTS") == "1", "requires the built native desktop runtime/GPU")
class DesktopReferenceTests(unittest.TestCase):
    def test_assertion_failure_feedback_and_repair_in_both_languages(self):
        root = ROOT / "logs/agent-feedback" / str(uuid.uuid4())
        for language in ("Rust", "CSharp"):
            with self.subTest(language=language):
                project = ROOT / ".temp/agent-feedback" / root.name / language
                prepare(project, language)
                launch = json.loads((project / "reference-test.json").read_text(encoding="utf-8"))

                def execute(label, config, scene="ReferenceStart", ticks=90, operation="test"):
                    job_id = str(uuid.uuid4())
                    request = dict(jobId=job_id, project=str(project), operation=operation, scene=scene, ticks=ticks,
                                   timeoutMs=120000, launchConfig=config)
                    report = run(project_tool(), request, root / language / label, label)[0]
                    validate_report(report, job_id, report["exitCode"])
                    return report

                def feedback(report, code, tick, snapshot):
                    self.assertEqual(report["status"], "failed", report)
                    items = [item for item in report["diagnostics"] if item["code"] == code]
                    self.assertEqual(len(items), 1, report)
                    item = items[0]
                    detail = json.loads(item["message"])
                    self.assertEqual(detail["tick"], tick)
                    artifact = Path(item["source"]["file"])
                    self.assertEqual(artifact.name, snapshot)
                    records = [record for record in report["artifacts"] if Path(record["path"]) == artifact]
                    self.assertEqual(len(records), 1, report)
                    self.assertEqual(records[0]["bytes"], artifact.stat().st_size)
                    state = json.loads(artifact.read_text(encoding="utf-8"))
                    self.assertFalse(state["assertions"][detail["assertionIndex"]]["passed"])
                    return detail

                wrong = json.loads(json.dumps(launch))
                wrong["assertions"] = [
                    dict(entity="Result", type="ReferenceResult", property="score", equals=999),
                    dict(entity="Absent", type="ReferenceResult", property="score", equals=0),
                    dict(entity="Result", type="AbsentType", property="score", equals=0),
                    dict(entity="Result", type="ReferenceResult", property="absentProperty", equals=None)]
                report = execute("final-failure", wrong)
                for reason in ("VALUE_MISMATCH", "MISSING_ENTITY", "MISSING_TYPE", "MISSING_PROPERTY"):
                    detail = feedback(report, "GAMEPLAY_ASSERTION_" + reason, 90, "state.json")
                    if reason == "VALUE_MISMATCH":
                        self.assertEqual(detail["expected"], 999)
                        self.assertEqual(detail["actual"], 20)
                        self.assertTrue(detail["actualPresent"])
                    else:
                        self.assertFalse(detail["actualPresent"])

                wrong = json.loads(json.dumps(launch))
                wrong["checkpoints"][0]["assertions"] = [dict(entity="Counter", type="$engine", property="uiText", equals="wrong")]
                report = execute("checkpoint-failure", wrong)
                detail = feedback(report, "GAMEPLAY_ASSERTION_VALUE_MISMATCH", 10, "checkpoint-10.json")
                self.assertEqual(detail["actual"], "Score 10")
                self.assertFalse(any(Path(item["path"]).name == "state.json" for item in report["artifacts"]))

                repaired = execute("repair", launch)
                self.assertEqual(repaired["status"], "passed", repaired)
                self.assertFalse(repaired["diagnostics"], repaired)
                for artifact in repaired["artifacts"]:
                    if Path(artifact["path"]).name in ("state.json", "checkpoint-10.json", "checkpoint-20.json", "checkpoint-59.json"):
                        self.assertEqual(artifact["bytes"], Path(artifact["path"]).stat().st_size)

                result_scene = project / "scenes/ReferenceResult.scene.toml"
                result_scene.write_text(result_scene.read_text(encoding="utf-8") + "\n[[entities]]\nname = 'Result'\nposition = [0.0, 0.0, 0.0]\nscale = [1.0, 1.0, 1.0]\n", encoding="utf-8")
                duplicate = json.loads(json.dumps(launch))
                duplicate["assertions"] = [dict(entity="Result", type="$engine", property="exists", equals=True)]
                detail = feedback(execute("ambiguous", duplicate), "GAMEPLAY_ASSERTION_AMBIGUOUS_TARGET", 90, "state.json")
                self.assertEqual(detail["matchCount"], 2)
                self.assertFalse(detail["actualPresent"])

                (project / "scenes/Feedback.scene.toml").write_text("[scene]\nversion = 16\nkind = '2d'\nfeatures = ['physics_2d']\n[[entities]]\nname = 'Marker'\n", encoding="utf-8")
                headless = dict(profile="native-headless-2d-v1", assertions=[dict(entity="Marker", type="$engine", property="exists", equals=False)])
                detail = feedback(execute("headless-failure", headless, "Feedback", 3, "run"), "GAMEPLAY_ASSERTION_VALUE_MISMATCH", 3, "state.json")
                self.assertEqual(detail["actual"], True)
                headless["assertions"][0]["equals"] = True
                self.assertEqual(execute("headless-repair", headless, "Feedback", 3, "run")["status"], "passed")

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
