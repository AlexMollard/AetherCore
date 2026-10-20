import unittest

from run import validate_probe


class ProbeValidationTests(unittest.TestCase):
    def valid(self):
        return dict(profile='csharp-wasm-interop-v1', status='passed', runtime='10.0.9',
                    scalar=True, utf8=True, structure=True, callbacks=True, reflection=True)

    def test_requires_all_observed_contract_checks(self):
        for key in ('scalar', 'utf8', 'structure', 'callbacks', 'reflection'):
            report = self.valid()
            del report[key]
            with self.assertRaises(ValueError):
                validate_probe(report)

    def test_rejects_wrong_runtime_or_fake_boolean(self):
        for key, value in (('runtime', '10.0.13'), ('callbacks', 'true'), ('status', 'failed')):
            report = self.valid()
            report[key] = value
            with self.assertRaises(ValueError):
                validate_probe(report)

    def test_accepts_complete_real_probe(self):
        validate_probe(self.valid())


if __name__ == '__main__':
    unittest.main()
