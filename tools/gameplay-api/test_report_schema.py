import copy
import json
import pathlib
import unittest

from jsonschema import Draft202012Validator

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = json.loads((ROOT / "docs/schemas/agent-report-v1.schema.json").read_text())


class ReportSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        Draft202012Validator.check_schema(SCHEMA)
        cls.validator = Draft202012Validator(SCHEMA)

    def example(self, status):
        return json.loads((ROOT / f"docs/schemas/examples/agent-report-v1-{status}.json").read_text())

    def test_serializer_examples_validate(self):
        for status in ("passed", "running", "failed"):
            with self.subTest(status=status):
                self.validator.validate(self.example(status))

    def test_success_with_errors_or_skipped_checks_is_rejected(self):
        passed = self.example("passed")
        passed["diagnostics"] = self.example("failed")["diagnostics"]
        self.assertFalse(self.validator.is_valid(passed))
        passed = self.example("passed")
        passed["requiredChecks"] = [{"name": "real-gpu", "status": "skipped"}]
        self.assertFalse(self.validator.is_valid(passed))

    def test_exit_code_and_source_location_semantics(self):
        running = self.example("running")
        running["exitCode"] = 0
        self.assertFalse(self.validator.is_valid(running))
        failed = self.example("failed")
        failed["diagnostics"] = []
        self.assertFalse(self.validator.is_valid(failed))
        failed = self.example("failed")
        failed["diagnostics"][0]["source"] = {"file": "player.rs", "column": 1}
        self.assertFalse(self.validator.is_valid(failed))

    def test_cancelled_and_skipped_are_distinct_terminal_outcomes(self):
        for status, code in (("cancelled", 130), ("skipped", 3)):
            item = self.example("running")
            item.update(status=status, exitCode=code)
            self.validator.validate(item)
            changed = copy.deepcopy(item)
            changed["exitCode"] = 0
            self.assertFalse(self.validator.is_valid(changed))


if __name__ == "__main__":
    unittest.main()
