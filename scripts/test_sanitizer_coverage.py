from pathlib import Path
import unittest
import check_sanitizer_coverage as coverage
import verify_asan_heap


class SanitizerCoverageTests(unittest.TestCase):
    def test_crashing_child_requires_real_asan_diagnostic_and_ready_marker(self):
        ready = 'ASAN engine allocation probe ready\n'
        verify_asan_heap.validate_result(0, ready, False)
        verify_asan_heap.validate_result(1, ready + 'ERROR: AddressSanitizer: heap-buffer-overflow', True)
        for exit_code, output in ((0, ready), (1, ready + 'access violation'), (1, 'AddressSanitizer: heap-buffer-overflow')):
            with self.subTest(output=output), self.assertRaises(ValueError):
                verify_asan_heap.validate_result(exit_code, output, True)
    def test_original_asset_boundary_omission_is_rejected(self):
        repo = Path(__file__).resolve().parent.parent
        commands = [dict(file=str(repo / 'tools/assetpack/BakeOutputs.cpp'), directory=str(repo), command='clang-cl /c BakeOutputs.cpp'),
                    dict(file=str(repo / 'src/engine/assets/AssetCatalog.cpp'), directory=str(repo), command='clang-cl -fsanitize=address /c AssetCatalog.cpp')]
        checked, missing = coverage.missing_instrumentation(commands, repo, True)
        self.assertEqual(len(checked), 2)
        self.assertEqual(missing, ['tools/assetpack/BakeOutputs.cpp'])

    def test_stl_dependency_requires_matching_instrumentation(self):
        repo = Path(__file__).resolve().parent.parent
        commands = [dict(file=str(repo / 'build/probe/_deps/imgui-src/misc/cpp/imgui_stdlib.cpp'), directory=str(repo), command='cl /c imgui_stdlib.cpp')]
        _, missing = coverage.missing_instrumentation(commands, repo, True)
        self.assertEqual(len(missing), 1)
        commands[0]['command'] += ' /fsanitize=address'
        _, missing = coverage.missing_instrumentation(commands, repo, True)
        self.assertEqual(missing, [])
        commands[0]['command'] += ' /D_DISABLE_STRING_ANNOTATION'
        _, missing = coverage.missing_instrumentation(commands, repo, True)
        self.assertEqual(len(missing), 1)


if __name__ == '__main__':
    unittest.main()
