import unittest

from verify_physics_scratch import ASSERTION, SURVIVED, validate_result


class ScratchProbeResultTests(unittest.TestCase):
    control = f'PHYSICS_SCRATCH_PROBE_READY control\n{SURVIVED}\n'
    failure = (
        'PHYSICS_SCRATCH_PROBE_READY exhaust\n'
        'Physics scratch exhausted: request=9027776 arenaUsage=640/1024 '
        'fallbackUsage=0/0 highWater=640. Terminating: '
        'Jolt cannot roll back a partially executed step.\n' + ASSERTION)

    def test_accepts_normal_control_and_specific_failure(self):
        validate_result(0, self.control, 'control')
        validate_result(0x80000003, self.failure, 'exhaust')

    def test_rejects_arbitrary_crash_and_missing_test_selection(self):
        for output in ('unrelated crash', 'PHYSICS_SCRATCH_PROBE_READY exhaust\ncrash'):
            with self.assertRaises(ValueError):
                validate_result(1, output, 'exhaust')

    def test_rejects_survival_even_with_policy_diagnostic(self):
        with self.assertRaises(ValueError):
            validate_result(1, self.failure + SURVIVED, 'exhaust')

    def test_rejects_zero_exit_and_sanitizer_failure(self):
        with self.assertRaises(ValueError):
            validate_result(0, self.failure, 'exhaust')
        with self.assertRaises(ValueError):
            validate_result(1, self.failure + '\nERROR: AddressSanitizer', 'exhaust')

    def test_rejects_failed_control_and_nonexhausting_request(self):
        with self.assertRaises(ValueError):
            validate_result(1, self.control, 'control')
        with self.assertRaises(ValueError):
            validate_result(1, self.failure.replace('request=9027776', 'request=1'), 'exhaust')

    def test_rejects_sanitizer_diagnostics_alongside_policy_evidence(self):
        for marker in ('AddressSanitizer:DEADLYSIGNAL', 'LeakSanitizer: detected memory leaks',
                       'MemorySanitizer: use-of-uninitialized-value', 'ThreadSanitizer: data race',
                       'UndefinedBehaviorSanitizer: undefined-behavior', 'runtime error: invalid access'):
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                validate_result(1, self.failure + '\n' + marker, 'exhaust')


if __name__ == '__main__':
    unittest.main()
