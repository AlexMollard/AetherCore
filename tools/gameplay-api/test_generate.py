import copy
import importlib.util
import json
import pathlib
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
spec = importlib.util.spec_from_file_location("gameplay_generate", HERE / "generate.py")
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((HERE / "engine-calls.json").read_text())

    def test_checked_surface_matches_current_native_and_csharp_signatures(self):
        generator.check_parity(self.manifest, ROOT)

    def test_signature_drift_is_rejected(self):
        changed = copy.deepcopy(self.manifest)
        changed["functions"][0]["result"] = "i32"
        with self.assertRaisesRegex(ValueError, "aether_entity_create"):
            generator.check_parity(changed, ROOT)

    def test_duplicate_names_and_invalid_types_cannot_generate_declarations(self):
        changed = copy.deepcopy(self.manifest)
        changed["functions"].append(changed["functions"][0])
        with self.assertRaises(ValueError):
            generator.render_calls(changed)
        changed = copy.deepcopy(self.manifest)
        changed["functions"][0]["name"] = "injected();"
        with self.assertRaises(ValueError):
            generator.render_calls(changed)

    def test_generated_rust_abi_preserves_c_callbacks_and_fixed_width_fields(self):
        source = (ROOT / "src/engine/scripting/GameplayAbi.h").read_text()
        rust = generator.render_abi(source)
        self.assertIn('#[repr(C)]', rust)
        self.assertIn('pub world: u64', rust)
        self.assertIn('pub r#type: u32', rust)
        self.assertIn('pub entityValid: Option<unsafe extern "C" fn', rust)
        self.assertIn('pub fn aether_gameplay_query', rust)
        self.assertNotIn('pub struct AetherGameplayEntity {\n    pub world: usize', rust)


if __name__ == "__main__":
    unittest.main()
