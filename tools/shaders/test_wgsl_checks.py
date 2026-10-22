from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wgsl_checks import find_negated_vector_sums, validate_negated_vector_sums


class NegatedVectorSums(unittest.TestCase):
    def test_rejects_slang_emitted_negated_sums(self):
        # Verbatim Slang 2026.8 output for exp(-(a * w + b * w)) and -(a - b).
        for wgsl in ('return exp((vec3<f32>(0) - u_0.a_0.xyz * vec3<f32>(u_0.b_0.w) + u_0.b_0.xyz * vec3<f32>(u_0.b_0.w)));',
                     'var g = (vec3<f32>(0) - u_0.a_0.xyz - u_0.b_0.xyz);',
                     'f(1.0f,\n  vec4<f32>(0) - x + y);'):
            with self.subTest(wgsl=wgsl):
                self.assertEqual(len(find_negated_vector_sums(wgsl)), 1)
                with self.assertRaisesRegex(ValueError, 'WEB_SHADER_NEGATED_SUM'):
                    validate_negated_vector_sums(wgsl)

    def test_accepts_single_negated_terms(self):
        for wgsl in ('return exp((vec3<f32>(0) - extinctionBeta_0 * vec3<f32>(_S6)));',
                     'var a = vec3<f32>(0) - (b_0 + c_0) * m_0;',
                     'var a = f(vec3<f32>(0) - b_0, c_0 + d_0);',
                     'var a = (vec3<f32>(0) - g(b_0 - c_0, 1e-06f)) * h_0;',
                     'var s = exp(- (a_0 * b_0 + c_0));'):
            with self.subTest(wgsl=wgsl):
                self.assertEqual(find_negated_vector_sums(wgsl), [])
                validate_negated_vector_sums(wgsl)

    def test_reports_line_of_each_finding(self):
        wgsl = 'fn a() {}\nvar x = vec2<f32>(0) - p + q;\nvar y = vec2<f32>(0) - p;\nvar z = vec2<f32>(0) - p - q;\n'
        self.assertEqual([line for line, _ in find_negated_vector_sums(wgsl)], [2, 4])


if __name__ == '__main__':
    unittest.main()
