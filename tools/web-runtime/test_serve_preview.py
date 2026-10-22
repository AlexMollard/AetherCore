import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('serve_preview', Path(__file__).with_name('serve-preview.py'))
preview = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preview)


class GalleryCatalogTests(unittest.TestCase):
    def test_every_maintained_scene_is_described_and_controls_resolve(self):
        catalog = preview.load_catalog()
        areas = {key for key, _, _ in catalog['areas']}
        scenes = {path.name.removesuffix('.scene.toml') for path in preview.MAINTAINED.glob('*.scene.toml')}
        entries = catalog['scenes']
        controls = {entry['control'] for entry in entries.values() if entry.get('control')}
        for name in sorted(scenes - controls):
            with self.subTest(scene=name):
                self.assertIn(name, entries, 'add a gallery-catalog.json entry for this scene')
                entry = entries[name]
                self.assertIn(entry['area'], areas)
                self.assertIn(entry['status'], catalog['statuses'])
                self.assertTrue(entry['summary'] and entry['lookFor'] and entry['title'])
        # The build also stages playable scenes from projects/ (EmberRun, the gameplay reference).
        staged = {path.name.removesuffix('.scene.toml') for path in (preview.REPO / 'projects').glob('*/scenes/*.scene.toml')}
        for name, entry in entries.items():
            with self.subTest(entry=name):
                self.assertIn(name, scenes | staged, 'catalogue entry for a scene that no longer exists')
                if entry.get('control'):
                    self.assertIn(entry['control'], scenes)
                    self.assertNotIn(entry['control'], entries, 'controls are shown on their feature card')


if __name__ == '__main__':
    unittest.main()
