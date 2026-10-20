import tempfile
from pathlib import Path
import unittest

from dotnet_wasm import engine_arguments, installed_environment


class DotnetWasmTests(unittest.TestCase):
    def test_missing_pinned_sdk_is_actionable_and_does_not_install(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, '10.0.303'):
                installed_environment(Path(directory), Path(directory) / 'scratch')

    def test_engine_link_uses_measured_runtime_contract(self):
        arguments = engine_arguments(Path('/sdk'), Path('/engine/mono-compat.js'))
        text = ' '.join(arguments)
        self.assertIn('ASSERTIONS=2', text)
        self.assertIn('DEFAULT_TO_CXX=1', text)
        self.assertIn('WasmEnableExceptionHandling=false', text)
        self.assertIn('WasmCachePath=', text)


if __name__ == '__main__':
    unittest.main()
