import tempfile
from pathlib import Path
import unittest
from cook_project_materials import custom_materials


class ProjectMaterialCookTests(unittest.TestCase):
    def test_scans_explicit_custom_materials_and_skips_builtins(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root/'assets/materials'
            assets.mkdir(parents=True)
            (assets/'native.toml').write_text('[material]\nshader="shaders://molten.spv"\n')
            graph = assets/'graph.material.toml'
            graph.write_text('[material]\nshader="shaders://Graph.spv"\n[graph]\nnext_id=1\n')
            self.assertEqual(custom_materials(root), [graph])

    def test_duplicate_shader_owner_fails_before_any_cooking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root/'assets'
            assets.mkdir()
            for name in ('a.toml', 'b.toml'):
                (assets/name).write_text('[material]\nshader="shaders://Graph.spv"\n')
            with self.assertRaisesRegex(ValueError, 'WEB_SHADER_DUPLICATE'):
                custom_materials(root)

    def test_graph_cannot_silently_claim_standard_pipeline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'assets').mkdir()
            (root/'assets/graph.toml').write_text('[material]\nshader="shaders://gltf_mesh.spv"\n[graph]\nnext_id=1\n')
            with self.assertRaisesRegex(ValueError, 'WEB_SHADER_GRAPH_URI'):
                custom_materials(root)


if __name__ == '__main__':
    unittest.main()
