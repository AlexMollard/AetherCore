import importlib.util
from pathlib import Path
import unittest
import tempfile
from threading import Thread
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import urlopen

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


class PublishedProjectRoutingTests(unittest.TestCase):
    def test_nested_project_assets_stay_inside_declared_package(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            package = repo / 'projects/Published/Builds/Web/Published'
            package.mkdir(parents=True)
            (package / 'index.html').write_text('<title>Published project</title>', encoding='utf-8')
            (package / 'game.wasm').write_bytes(b'\0asm')
            (package.parent / 'secret.txt').write_text('outside package', encoding='utf-8')
            catalog = {'projects': {'Published': {'directory': 'projects/Published/Builds/Web/Published'}}}
            with patch.object(preview, 'REPO', repo), patch.object(preview, 'load_catalog', return_value=catalog):
                server = preview.ThreadingHTTPServer(('127.0.0.1', 0), preview.partial(preview.Preview, build=repo))
                thread = Thread(target=server.serve_forever, daemon=True)
                thread.start()
                base = f'http://127.0.0.1:{server.server_port}'
                try:
                    with urlopen(base + '/projects/Published/') as response:
                        self.assertIn(b'<title>Published project</title>', response.read())
                    with urlopen(base + '/projects/Published/game.wasm') as response:
                        self.assertEqual(response.headers.get_content_type(), 'application/wasm')
                    for route in ('/projects/Published/%2e%2e/secret.txt', '/projects/Unknown/game.wasm',
                                  '/projects/Published/%2fsecret.txt'):
                        with self.subTest(route=route), self.assertRaises(HTTPError) as error:
                            urlopen(base + route)
                        self.assertEqual(error.exception.code, 404)
                finally:
                    server.shutdown()
                    server.server_close()
                    thread.join()


if __name__ == '__main__':
    unittest.main()
