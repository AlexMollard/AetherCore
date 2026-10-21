"""Host the development runtime with a gallery; leave export shells unchanged."""

import argparse
from functools import partial
from html import escape
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlencode, urlsplit

REPO = Path(__file__).resolve().parents[2]
DESCRIPTIONS = {
    'EmberRun': 'A playable 2D platformer. Select Play, then use the keyboard.',
    'ReferenceStart': 'Rust gameplay, sprites, audio, persistence and scene transitions.',
    'ReferenceResult': 'The companion result scene for the gameplay reference.',
    'BrowserFeatures': 'Tilemaps, sprite animation, text and UI rendering.',
    'BrowserModels': 'Authored 3D models, linked materials and depth testing.',
    'BrowserMaterials': 'Material textures, mipmaps, transparency and face culling.',
    'BrowserSkin': 'Skinned models and animation playback.',
    'BrowserDependencies': 'Scene includes and prefab additions, removals and overrides.',
    'BrowserIncluded': 'The scene fragment used by the dependency example.',
    'BrowserSolid': 'Camera solid colour preserved through the HDR compositor.',
    'BrowserGradient': 'Camera colour stops blended in display space.',
    'BrowserSky': 'Native procedural sky gradient, horizon and ground colours.',
    'BrowserHdr': 'Half and eight units of scene radiance through native ACES tonemapping.',
    'BrowserSun': 'Directional sunlight on a rough dielectric surface.',
    'BrowserPoint': 'Point lighting with native inverse-square attenuation.',
    'BrowserSpot': 'Spotlight cone falloff on an authored 3D surface.',
    'BrowserPbrMaps': 'Tangent normals, occlusion and metallic/roughness texture maps.',
    'BrowserLight2D': 'A 2D lamp casts shadows through a tile artwork silhouette.',
    'BrowserBadLight': 'An invalid NaN light must fail before GPU frame submission.',
    'default': 'The reference project’s default scene.',
}
for kind, label in [('Sun', 'Directional'), ('Point', 'Point'), ('Spot', 'Spot'), ('Mask', 'Alpha-cutout'), ('Skin', 'Skinned-model')]:
    for suffix, state in [('On', 'enabled'), ('Off', 'disabled')]:
        DESCRIPTIONS[f'BrowserShadow{kind}{suffix}'] = f'{label} shadow comparison, with receiver shadows {state}.'


def gallery(build):
    groups = {'Play and explore': [], 'Rendering and scene fixtures': [], 'Expected failures': []}
    for scene in sorted((build / 'reference-project/scenes').glob('*.scene.toml')):
        name = scene.name.removesuffix('.scene.toml')
        group = ('Expected failures' if name.startswith('BrowserBad') else
                 'Rendering and scene fixtures' if name.startswith('Browser') else 'Play and explore')
        description = DESCRIPTIONS.get(name, 'An intentionally invalid asset or scene. Opening this example should display a diagnostic.'
                                       if group == 'Expected failures' else 'An authored browser scene.')
        url = '/?' + urlencode({'reference': '1', 'scene': name})
        groups[group].append(f'<a class="card" href="{escape(url)}"><h3>{escape(name)}</h3><p>{escape(description)}</p><span>Open example &rarr;</span></a>')
    groups['Rendering and scene fixtures'].append(
        '<a class="card" href="/?scene=mesh3d"><h3>Mesh foundation</h3><p>Procedural 3D geometry, depth testing and the sprite overlay.</p><span>Open example &rarr;</span></a>')
    sections = ''.join(f'<section><h2>{heading}</h2><div class="grid">{"".join(cards)}</div></section>'
                       for heading, cards in groups.items() if cards)
    template = (REPO / 'resources/web/gallery.html').read_text(encoding='utf-8')
    return template.replace('<!-- EXAMPLES -->', sections).encode('utf-8')


class Preview(SimpleHTTPRequestHandler):
    def __init__(self, *args, build, **kwargs):
        self.build = build
        super().__init__(*args, directory=str(build / 'web'), **kwargs)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def send_head(self):
        target = urlsplit(self.path)
        if target.path == '/' and not target.query:
            from io import BytesIO
            content = gallery(self.build)
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(content)))
            self.end_headers()
            return BytesIO(content)
        return super().send_head()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=REPO / 'build/webgpu-browser')
    parser.add_argument('--port', type=int, default=18989)
    args = parser.parse_args()
    if not (args.build / 'web/index.html').is_file():
        parser.error('Build the Web runtime before hosting its gallery')
    ThreadingHTTPServer(('127.0.0.1', args.port), partial(Preview, build=args.build.resolve())).serve_forever()
