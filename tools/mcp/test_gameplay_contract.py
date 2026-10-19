"""Control transport regression tests; no running engine or third-party packages."""

import importlib.util
import json
from pathlib import Path
import subprocess
import unittest
from unittest import mock


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("aethercore_mcp", HERE / "aethercore_mcp.py")
mcp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mcp)


def blocked_payload():
    return {
        "ok": False,
        "state": "editing",
        "message": "Rust backend unavailable: 世界/level\u0000.rs",
        "report": {
            "schemaVersion": 1,
            "jobId": "play-1",
            "status": "failed",
            "phase": "prerequisites",
            "target": "desktop",
            "language": "rust",
            "profile": "default",
            "diagnostics": [{"code": "gameplay.backend_unavailable", "severity": "error", "message": "世界/level\u0000.rs"}],
            "toolchains": [],
            "artifacts": [],
            "requiredChecks": [],
            "durationMs": 0,
            "exitCode": 1,
        },
    }


class GameplayTransportTests(unittest.TestCase):
    def setUp(self):
        self.path_patch = mock.patch.object(mcp, "_ctl_path", return_value=str(HERE / "aethercore_mcp.py"))
        self.path_patch.start()
        self.addCleanup(self.path_patch.stop)

    def call_ctl(self, payload, code=0, stderr="", params=None):
        stdout = json.dumps(payload, ensure_ascii=False) if not isinstance(payload, str) else payload
        process = subprocess.CompletedProcess(["aether-ctl"], code, stdout, stderr)
        with mock.patch.object(mcp.subprocess, "run", return_value=process) as run:
            result = mcp._ctl("engine.play", params)
        return result, run.call_args

    def test_structured_failure_retains_the_complete_report(self):
        payload = blocked_payload()
        result, _ = self.call_ctl(payload, code=1)
        self.assertEqual(result, {"result": payload, "error": payload["message"]})

    def test_structured_failure_is_an_mcp_error_with_original_json_content(self):
        payload = blocked_payload()
        tools = [{"name": "engine_play", "wire": "engine.play"}]
        process = subprocess.CompletedProcess(["aether-ctl"], 1, json.dumps(payload, ensure_ascii=False), "")
        with mock.patch.object(mcp, "_engine_tools", return_value=tools), mock.patch.object(mcp.subprocess, "run", return_value=process):
            response = mcp._handle({"id": 7, "method": "tools/call", "params": {"name": "engine_play"}})
        self.assertTrue(response["result"]["isError"])
        self.assertEqual(json.loads(response["result"]["content"][0]["text"]), payload)

    def test_read_only_capability_query_succeeds_with_a_failed_prerequisite(self):
        payload = {"language": "rust", "profile": "default", "prerequisiteReport": blocked_payload()["report"]}
        result, _ = self.call_ctl(payload)
        self.assertEqual(result, {"result": payload})
        with mock.patch.object(mcp, "_call_tool", return_value=result):
            response = mcp._handle({"id": 8, "method": "tools/call", "params": {"name": "gameplay_capabilities"}})
        self.assertFalse(response["result"]["isError"])
        self.assertEqual(json.loads(response["result"]["content"][0]["text"]), payload)

    def test_unicode_nul_and_input_json_use_explicit_utf8(self):
        payload = {"name": "世界\u0000level"}
        result, call = self.call_ctl(payload, params=payload)
        self.assertEqual(result["result"], payload)
        self.assertEqual(json.loads(call.kwargs["input"]), payload)
        self.assertEqual(call.kwargs.get("encoding"), "utf-8")
        self.assertTrue(call.kwargs["text"])

    def test_legacy_failure_preserves_stderr_and_mcp_error_text(self):
        result, _ = self.call_ctl("", code=1, stderr="error: unknown method\n")
        self.assertEqual(result, {"error": "error: unknown method"})
        with mock.patch.object(mcp, "_call_tool", return_value=result):
            response = mcp._handle({"id": 9, "method": "tools/call", "params": {"name": "unknown"}})
        self.assertTrue(response["result"]["isError"])
        self.assertEqual(json.loads(response["result"]["content"][0]["text"]), "error: unknown method")

    def test_non_contract_json_failure_keeps_legacy_behavior(self):
        for payload in ({"ok": False, "message": "legacy rejection"}, {"ok": False, "report": {"schemaVersion": 2, "status": "failed", "exitCode": 1}}, [], 42):
            with self.subTest(payload=payload):
                result, _ = self.call_ctl(payload, code=1, stderr="legacy failure")
                self.assertEqual(result, {"error": "legacy failure"})

    def test_failure_contract_requires_false_ok_and_matching_terminal_exit_code(self):
        for field, value in (("ok", True), ("ok", 0), ("status", "running"), ("status", []), ("exitCode", 0), ("exitCode", True), ("schemaVersion", True)):
            payload = blocked_payload()
            if field == "ok":
                payload[field] = value
            else:
                payload["report"][field] = value
            with self.subTest(field=field, value=value):
                result, _ = self.call_ctl(payload, code=1, stderr="legacy failure")
                self.assertEqual(result, {"error": "legacy failure"})

    def test_cancelled_and_skipped_terminal_reports_remain_structured(self):
        for status, code in (("cancelled", 130), ("skipped", 3)):
            payload = blocked_payload()
            payload["report"].update(status=status, exitCode=code)
            result, _ = self.call_ctl(payload, code=code)
            self.assertEqual(result, {"result": payload, "error": payload["message"]})


if __name__ == "__main__":
    unittest.main()
