"""Host the development runtime with a gallery; leave export shells unchanged."""

import argparse
from functools import partial
from html import escape
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import subprocess
from urllib.parse import parse_qs, quote, unquote, urlencode, urlsplit

REPO = Path(__file__).resolve().parents[2]
CATALOG = REPO / 'resources/web/gallery-catalog.json'
MAINTAINED = REPO / 'resources/web/reference/scenes'


def load_catalog():
    return json.loads(CATALOG.read_text(encoding='utf-8'))


def changed_scenes():
    """Maintained scenes with uncommitted edits: the examples currently being worked on."""
    try:
        result = subprocess.run(['git', 'status', '--porcelain', '--', str(MAINTAINED)], cwd=REPO,
                                capture_output=True, text=True, timeout=5, check=True)
    except (OSError, subprocess.SubprocessError):
        return set()
    return {Path(line[3:].strip().strip('"')).name.removesuffix('.scene.toml') for line in result.stdout.splitlines()
            if line[3:].strip().endswith('.scene.toml')}


def scene_link(name):
    return '/?' + urlencode({'reference': '1', 'scene': name})


def card(name, entry, catalog, changed, updated):
    statuses = catalog['statuses']
    status = entry.get('status', 'fixture')
    area = entry.get('area', 'unsorted')
    control = entry.get('control')
    badges = [f'<span class="badge {escape(status)}">{escape(statuses.get(status, status))}</span>']
    if name in changed or (control and control in changed):
        badges.append('<span class="badge changed">Uncommitted changes</span>')
    if entry.get('missing'):
        badges.append('<span class="badge missing">Needs description</span>')
    links = [f'<a class="open" href="{escape(entry.get("url") or scene_link(name))}">Open example &rarr;</a>']
    if control:
        links.append(f'<a class="control" href="{escape(scene_link(control))}" title="{escape(control)}">Open control (feature off)</a>')
    look = f'<p class="look"><b>What to look for:</b> {escape(entry["lookFor"])}</p>' if entry.get('lookFor') else ''
    names = name + (' ' + control if control else '')
    search = ' '.join([names, entry.get('title', ''), entry.get('summary', ''), entry.get('lookFor', ''), statuses.get(status, status)]).lower()
    return (f'<article class="card" data-area="{escape(area)}" data-status="{escape(status)}" data-updated="{updated}" data-search="{escape(search)}">'
            f'<div class="badges">{"".join(badges)}</div><h3>{escape(entry.get("title", name))}</h3>'
            f'<code>{escape(names)}</code><p>{escape(entry.get("summary", ""))}</p>{look}<div class="links">{"".join(links)}</div></article>')


def project_asset(name, relative='index.html'):
    entry = load_catalog().get('projects', {}).get(name)
    if entry is None:
        return None
    root = (REPO / entry['directory']).resolve()
    if not root.is_relative_to((REPO / 'projects').resolve()):
        return None
    target = (root / relative).resolve()
    if not target.is_relative_to(root):
        return None
    if target.is_dir():
        target = (target / 'index.html').resolve()
    return target if target.is_relative_to(root) and target.is_file() else None


def gallery(build):
    catalog = load_catalog()
    scenes = {path.name.removesuffix('.scene.toml'): path for path in sorted((build / 'reference-project/scenes').glob('*.scene.toml'))}
    controls = {entry['control'] for entry in catalog['scenes'].values() if entry.get('control')}
    changed = changed_scenes()
    entries = {}
    for name, path in scenes.items():
        if name in controls:
            continue
        entry = catalog['scenes'].get(name)
        if entry is None:
            # A new scene without a catalogue entry still shows up, flagged, rather than hiding.
            entry = dict(area='failures' if name.startswith('BrowserBad') else 'unsorted', status='in-progress', title=name,
                         summary='No catalogue entry yet in resources/web/gallery-catalog.json.', missing=True)
        source = MAINTAINED / path.name
        updated = int(max(path.stat().st_mtime, source.stat().st_mtime if source.is_file() else 0))
        entries[name] = (entry, updated)
    entries['mesh3d'] = (dict(area='models', status='qualified', title='Mesh foundation', url='/?scene=mesh3d',
                              summary='Procedural 3D geometry, depth testing and the sprite overlay from the first WebGPU milestone.',
                              lookFor='Procedural primitives sort by depth and keep their shape under non-uniform scale; the 2D overlay stays on top.'), 0)
    for name, entry in catalog.get('projects', {}).items():
        index = project_asset(name)
        if index:
            entries[name] = (dict(entry, url='/projects/' + quote(name, safe='') + '/'), int(index.stat().st_mtime))
    working = sorted((name for name, (entry, _) in entries.items()
                      if entry.get('status') == 'in-progress' or name in changed or entry.get('control') in changed),
                     key=lambda name: -entries[name][1])
    render = lambda name: card(name, entries[name][0], catalog, changed, entries[name][1])
    areas = [*catalog['areas'], ['unsorted', 'Unsorted', 'Scenes that have no catalogue entry yet.']]
    sections = []
    if working:
        sections.append('<section class="working" data-section="working"><h2>Being worked on</h2>'
                        '<p class="hint">In-progress examples and scenes with uncommitted edits, most recently changed first.</p>'
                        f'<div class="grid">{"".join(render(name) for name in working)}</div></section>')
    for key, heading, blurb in areas:
        names = sorted((name for name, (entry, _) in entries.items() if entry.get('area') == key), key=lambda name: entries[name][0].get('title', name))
        if names:
            sections.append(f'<section data-section="{escape(key)}"><h2>{escape(heading)} <small>{len(names)}</small></h2>'
                            f'<p class="hint">{escape(blurb)}</p><div class="grid">{"".join(render(name) for name in names)}</div></section>')
    chips = ''.join(f'<button type="button" data-filter="{escape(key)}">{escape(heading)}</button>' for key, heading, _ in areas
                    if any(entry.get('area') == key for entry, _ in entries.values()))
    template = (REPO / 'resources/web/gallery.html').read_text(encoding='utf-8')
    total = f'{len(entries)} examples, {len(working)} being worked on'
    return (template.replace('<!-- FILTERS -->', chips).replace('<!-- COUNT -->', escape(total))
            .replace('<!-- EXAMPLES -->', ''.join(sections)).encode('utf-8'))


def example_details(name):
    """Catalogue entry for one preview scene, resolving a control scene to its feature card."""
    catalog = load_catalog()
    scenes = catalog['scenes']
    feature = next((key for key, entry in scenes.items() if entry.get('control') == name), None)
    if name == 'mesh3d':
        entry = dict(area='models', status='qualified', title='Mesh foundation',
                     summary='Procedural 3D geometry, depth testing and the sprite overlay from the first WebGPU milestone.',
                     lookFor='Procedural primitives sort by depth and keep their shape under non-uniform scale; the 2D overlay stays on top.')
    else:
        entry = scenes.get(feature or name)
    if entry is None:
        return dict(scene=name, title=name, summary='No catalogue entry yet in resources/web/gallery-catalog.json.')
    areas = {key: heading for key, heading, _ in catalog['areas']}
    details = dict(scene=name, title=entry['title'], summary=entry['summary'], lookFor=entry.get('lookFor', ''),
                   area=areas.get(entry['area'], entry['area']), areaKey=entry['area'],
                   status=catalog['statuses'].get(entry['status'], entry['status']), statusKey=entry['status'])
    on = feature or name
    off = scenes.get(on, {}).get('control')
    if off:
        details.update(isControl=feature is not None, feature=on, control=off, featureUrl=scene_link(on), controlUrl=scene_link(off))
    return details


def example_page(build, name):
    # The source shell, so shell edits show without a Web rebuild; the bridge mirrors the build's CMake substitution.
    bridge = '<script src="test-bridge.js"></script>' if (build / 'web/test-bridge.js').is_file() else ''
    page = (REPO / 'resources/web/index.html').read_text(encoding='utf-8').replace('@AETHER_WEB_BRIDGE_SCRIPT@', bridge)
    data = json.dumps(dict(example_details(name), galleryUrl='/')).replace('</', '<\\/')
    # Development previews only: exported packages never pass through this server.
    return page.replace('<script src="project.js"></script>',
                        f'<script>globalThis.aetherExample={data};</script><script src="project.js"></script>', 1).encode('utf-8')


class Preview(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, '.js': 'text/javascript',
                      '.mjs': 'text/javascript', '.wasm': 'application/wasm'}

    def __init__(self, *args, build, **kwargs):
        self.build = build
        super().__init__(*args, directory=str(build / 'web'), **kwargs)

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def send_head(self):
        target = urlsplit(self.path)
        query = parse_qs(target.query)
        if target.path.startswith('/projects/'):
            parts = unquote(target.path).split('/', 3)
            if len(parts) < 4:
                self.send_response(301)
                self.send_header('Location', target.path + '/')
                self.end_headers()
                return None
            asset = project_asset(parts[2], parts[3] or 'index.html')
            if asset is None:
                self.send_error(404, 'Unknown project or package asset')
                return None
            stream = asset.open('rb')
            self.send_response(200)
            self.send_header('Content-Type', self.guess_type(str(asset)))
            self.send_header('Content-Length', str(asset.stat().st_size))
            self.end_headers()
            return stream
        # The gallery owns '/' and its own ?area= filter links; any other query opens the runtime.
        if target.path == '/' and set(query) <= {'area'}:
            return self.html(gallery(self.build))
        if target.path in ('/', '/index.html') and 'scene' in query:
            return self.html(example_page(self.build, query['scene'][0]))
        if target.path == '/platform.js':
            # The shell comes from source, so its platform script must too.
            return self.respond((REPO / 'resources/web/platform.js').read_bytes(), 'text/javascript; charset=utf-8')
        return super().send_head()

    def html(self, content):
        return self.respond(content, 'text/html; charset=utf-8')

    def respond(self, content, content_type):
        from io import BytesIO
        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(content)))
        self.end_headers()
        return BytesIO(content)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=REPO / 'build/webgpu-browser')
    parser.add_argument('--port', type=int, default=18989)
    args = parser.parse_args()
    if not (args.build / 'web/index.html').is_file():
        parser.error('Build the Web runtime before hosting its gallery')
    ThreadingHTTPServer(('127.0.0.1', args.port), partial(Preview, build=args.build.resolve())).serve_forever()
