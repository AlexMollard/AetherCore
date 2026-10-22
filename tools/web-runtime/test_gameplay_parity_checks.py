import copy
import importlib.util
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('gameplay_parity', Path(__file__).with_name('gameplay-parity-checks.py'))
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)


def matrix(x=0.0, y=0.0, z=0.0, angle=0.0, scale=(1.0, 1.0, 1.0)):
    c, s = math.cos(angle), math.sin(angle)
    return [c*scale[0], s*scale[0], 0, 0, -s*scale[1], c*scale[1], 0, 0, 0, 0, scale[2], 0, x, y, z, 1]


def engine(name, **kwargs):
    m = matrix(**kwargs)
    return dict(entity=name, type='$engine', properties=dict(exists=True, position=m[12:15], localToWorld=m))


def script(name, **properties):
    return dict(entity=name, type='Script', properties=properties)


def state(*records):
    return dict(entities=list(records))


class TransformTests(unittest.TestCase):
    def test_decomposition_recovers_position_scale_and_rotation(self):
        result = parity.transform_of(matrix(1, 2, 3, math.pi/2, (2.0, 3.0, 4.0)))
        self.assertEqual(result['position'], [1, 2, 3])
        for actual, expected in zip(result['scale'], (2.0, 3.0, 4.0)):
            self.assertAlmostEqual(actual, expected)
        for actual, expected in zip(result['rotation'], (0, 0, math.sin(math.pi/4), math.cos(math.pi/4))):
            self.assertAlmostEqual(actual, expected)

    def test_quaternion_sign_is_canonical(self):
        # 180 degrees about Z has w == 0: both signs describe it, only one may be reported.
        a = parity.transform_of(matrix(angle=math.pi))['rotation']
        self.assertGreaterEqual(a[2], 0)
        self.assertTrue(parity.transform_of(matrix(angle=-math.pi/2))['rotation'][3] > 0)

    def test_malformed_matrix_fails(self):
        with self.assertRaises(ValueError):
            parity.transform_of([1, 2, 3])


class CompareTests(unittest.TestCase):
    def compare(self, native, web, tolerance=1e-5, tick=10):
        return parity.compare_records(tick, parity.normalize(native), parity.normalize(web), tolerance)

    def test_identical_states_match_and_count_fields(self):
        native = state(engine('Player', x=1.5), script('Player', score=3, label='a'))
        mismatches, compared = self.compare(native, copy.deepcopy(native))
        self.assertEqual(mismatches, [])
        self.assertGreater(compared, 10)

    def test_mismatch_reports_entity_tick_field_native_web(self):
        native, web = state(engine('Player', x=1.0)), state(engine('Player', x=1.0 + 1e-3))
        (item,), _ = self.compare(native, web, tick=30)
        self.assertEqual((item['entity'], item['tick'], item['field']), ('Player', 30, 'position.x'))
        self.assertEqual(item['native'], 1.0)
        self.assertAlmostEqual(item['web'], 1.001)

    def test_tolerance_boundary_is_inclusive(self):
        base = state(engine('P', x=0.0))
        at, over = state(engine('P', x=1e-5)), state(engine('P', x=1.0000001e-5))
        self.assertEqual(self.compare(base, at)[0], [])
        self.assertEqual([m['field'] for m in self.compare(base, over)[0]], ['position.x'])

    def test_zero_tolerance_fails_on_any_drift(self):
        base, web = state(engine('P', x=2.0)), state(engine('P', x=2.0 + 2**-20))
        self.assertEqual(self.compare(base, web, 0.0)[0][0]['field'], 'position.x')
        self.assertEqual(self.compare(base, copy.deepcopy(base), 0.0)[0], [])

    def test_rotation_and_scale_drift_detected(self):
        native = state(engine('P', angle=0.5, scale=(1.0, 1.0, 1.0)))
        web = state(engine('P', angle=0.5001, scale=(1.01, 1.0, 1.0)))
        fields = {m['field'] for m in self.compare(native, web)[0]}
        self.assertIn('rotation.z', fields)
        self.assertIn('scale.x', fields)

    def test_missing_entity_on_either_side(self):
        both = state(engine('A'), engine('B'))
        only_a = state(engine('A'))
        (missing_web,), _ = self.compare(both, only_a)
        self.assertEqual((missing_web['entity'], missing_web['field'], missing_web['native'], missing_web['web']), ('B', 'exists', True, False))
        (missing_native,), _ = self.compare(only_a, both)
        self.assertEqual((missing_native['native'], missing_native['web']), (False, True))

    def test_script_properties_exact_ints_tolerant_floats_and_vectors(self):
        native = state(script('P', ticks=10, speed=0.5, pos=[0.0, 1.0, 2.0], label='x', hit=True))
        same = state(script('P', ticks=10, speed=0.5 + 1e-7, pos=[0.0, 1.0 + 1e-7, 2.0], label='x', hit=True))
        self.assertEqual(self.compare(native, same)[0], [])
        bad = state(script('P', ticks=11, speed=0.5, pos=[0.0, 1.1, 2.0], label='y', hit=False))
        fields = {m['field'] for m in self.compare(native, bad)[0]}
        self.assertEqual(fields, {'ticks', 'pos[1]', 'label', 'hit'})

    def test_missing_script_property_is_reported(self):
        (item,), _ = self.compare(state(script('P', a=1, b=2)), state(script('P', a=1)))
        self.assertEqual((item['field'], item['web']), ('b', '<absent>'))

    def test_bool_is_not_a_number(self):
        (item,), _ = self.compare(state(script('P', flag=True)), state(script('P', flag=1)))
        self.assertEqual(item['field'], 'flag')

    def test_runtime_compares_scene_only_and_uses_web_scene(self):
        native = state(dict(entity='$runtime', type='$engine', properties=dict(scene='ReferenceStart', audioStarted=1)))
        web = dict(scene='ReferenceStart', entities=[])
        left, right = parity.normalize(native), parity.normalize(web, web['scene'])
        self.assertEqual(parity.compare_records(1, left, right, 1e-5)[0], [])
        right = parity.normalize(web, 'ReferenceResult')
        self.assertEqual(parity.compare_records(1, left, right, 1e-5)[0][0]['field'], 'scene')

    def test_duplicate_names_must_match_in_count(self):
        (item,), _ = self.compare(state(engine('A'), engine('A')), state(engine('A')))
        self.assertEqual(item['field'], 'exists')


class CaseTests(unittest.TestCase):
    def test_case_passes_and_carries_transforms_for_both_backends(self):
        runtime = dict(entity='$runtime', type='$engine', properties=dict(scene='S', audioStarted=0))
        native = {t: state(engine('Player', x=t/10), runtime) for t in (1, 10)}
        web = {t: dict(scene='S', tick=t, entities=[copy.deepcopy(native[t]['entities'][0])]) for t in (1, 10)}
        case = parity.compare_case('S', [1, 10], native, web, 1e-5)
        self.assertEqual(case['status'], 'passed')
        self.assertEqual(case['checkpoints'][1]['nativeTransforms']['Player']['position'][0], 1.0)
        self.assertEqual(case['checkpoints'][1]['webTransforms'], case['checkpoints'][1]['nativeTransforms'])

    def test_missing_web_checkpoint_fails(self):
        native = {1: state(engine('Player')), 10: state(engine('Player'))}
        web = {1: dict(entities=native[1]['entities'])}
        case = parity.compare_case('S', [1, 10], native, web, 1e-5)
        self.assertEqual(case['status'], 'failed')
        self.assertEqual(case['checkpoints'][1]['mismatches'][0]['field'], 'checkpoint')

    def test_perturbed_web_state_fails_a_previously_passing_case(self):
        runtime = dict(entity='$runtime', type='$engine', properties=dict(scene='S', audioStarted=0))
        native = {t: state(engine('Player', x=t/10), runtime) for t in (1, 10)}
        web = {t: dict(scene='S', entities=[copy.deepcopy(native[t]['entities'][0])]) for t in (1, 10)}
        self.assertEqual(parity.compare_case('S', [1, 10], native, web, 1e-5)['status'], 'passed')
        case = parity.compare_case('S', [1, 10], native, parity.perturb(web, 1e-3), 1e-5)
        self.assertEqual(case['status'], 'failed')
        self.assertEqual(case['checkpoints'][0]['mismatches'][0]['field'], 'position.x')
        # The same drift passes under a looser recorded tolerance.
        self.assertEqual(parity.compare_case('S', [1, 10], native, parity.perturb(web, 1e-3), 1e-2)['status'], 'passed')


class NativeSelectionTests(unittest.TestCase):
    @staticmethod
    def native(simulated):
        return state(dict(entity='$runtime', type='$engine', properties=dict(scene='S', simulatedTicks=simulated)))

    def test_selects_checkpoint_that_completed_requested_simulation_steps(self):
        raw = {1: self.native(0), 2: self.native(1), 10: self.native(9), 11: self.native(10)}
        selected, mapping = parity.select_native(raw, [1, 10])
        self.assertEqual(mapping, {1: 2, 10: 11})
        self.assertEqual(parity.simulated_ticks(selected[10]), 10)

    def test_unmatched_simulation_count_is_missing_not_guessed(self):
        selected, mapping = parity.select_native({5: self.native(3), 6: self.native(4)}, [5])
        self.assertEqual((selected[5], mapping[5]), (None, None))

    def test_native_checkpoint_ticks_cover_the_warmup_neighbour(self):
        self.assertEqual(parity.native_checkpoint_ticks([1, 10, 11]), [1, 2, 10, 11, 12])


class ValidationTests(unittest.TestCase):
    def test_checkpoints(self):
        self.assertEqual(parity.validate_checkpoints([60, 1, 10]), [1, 10, 60])
        for bad in ([], [0], [601], [1, 1], [1.5], 'x'):
            with self.assertRaises(ValueError):
                parity.validate_checkpoints(bad)

    def test_tolerance(self):
        self.assertEqual(parity.validate_tolerance(0), 0.0)
        for bad in (-1, float('nan'), float('inf'), '1e-5', True):
            with self.assertRaises(ValueError):
                parity.validate_tolerance(bad)

    def test_input(self):
        ok = [dict(tick=0, key='space', down=True)]
        self.assertEqual(parity.validate_input(ok, 60), ok)
        for bad in ([dict(tick=61, key='space', down=True)], [dict(tick=0, key='nope', down=True)],
                    [dict(tick=0, key='space')], [dict(tick=0, key='space', down=1)], {}):
            with self.assertRaises(ValueError):
                parity.validate_input(bad, 60)

    def test_key_codes_match_native_table(self):
        self.assertEqual([parity.key_code(k) for k in ('space', 'a', 'Z', '0', 'left', 'right')], [32, 65, 90, 48, 263, 262])


if __name__ == '__main__':
    unittest.main()
