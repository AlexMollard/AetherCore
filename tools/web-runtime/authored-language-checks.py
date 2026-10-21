"""Private authored-3D language qualification; never expands the released Web profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/project'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_browser(command, log_path):
    # Reuse the publisher's process-tree owner so a timeout or interrupted
    # qualifier also closes only its own Chromium children on Windows/Linux.
    from gameplay_jobs import _OwnedProcess
    command[0] = shutil.which(command[0]) or command[0]
    with log_path.open('wb') as log:
        process = _OwnedProcess(command, ROOT, log)
        deadline = time.monotonic() + 180
        try:
            while process.poll() is None:
                if time.monotonic() >= deadline:
                    process.kill()
                    raise TimeoutError('Private browser qualification exceeded 180 seconds')
                time.sleep(0.025)
            return process.poll()
        finally:
            process.close()


def cached_projects(path):
    result = {}
    if path.is_file():
        for job in json.loads(path.read_text(encoding='utf-8-sig')):
            report = job.get('report', {})
            if report.get('status') != 'passed':
                continue
            for artifact in report.get('artifacts', []):
                target = Path(artifact.get('path', ''))
                if target.name == 'manifest.json' and target.is_file():
                    result[report['language']] = target.parents[3]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog='Logs: logs/web3d/authored-languages/. Private stages remain under .temp for inspection.')
    parser.add_argument('--cached-report', type=Path, default=ROOT / 'logs/web3d/export-regression.json', help='Reuse verified private 2D publisher project roots; sources are refreshed before republishing.')
    parser.add_argument('--fresh', action='store_true', help='Use new private source clones rather than cached publisher projects.')
    parser.add_argument('--directory', type=Path, default=ROOT / '.temp/private-3d-qualification' / uuid.uuid4().hex)
    parser.add_argument('--report', type=Path, default=ROOT / 'logs/web3d/authored-languages/report.json')
    parser.add_argument('--packer', type=Path, default=Path(os.environ.get('AETHER_WEB_ASSETPACKER', ROOT / ('build/default/tools/RelWithDebInfo/AssetPacker.exe' if os.name == 'nt' else 'build/linux-clang/tools/AssetPacker'))))
    parser.add_argument('--playwright', default=os.environ.get('AETHER_PLAYWRIGHT_MODULE', 'playwright'))
    parser.add_argument('--executable', help='Chromium executable; defaults to installed hardware Chrome.')
    args = parser.parse_args()
    # The verified production publisher and the private browser runner use the
    # same explicitly selected browser tooling.
    os.environ['AETHER_PLAYWRIGHT_MODULE'] = args.playwright
    stage = args.directory.resolve()
    if not stage.is_relative_to((ROOT / '.temp').resolve()):
        parser.error('--directory must remain inside the repository .temp directory')
    stage.mkdir(parents=True, exist_ok=False)
    args.report = args.report.resolve()
    args.report.parent.mkdir(parents=True, exist_ok=True)
    report = dict(schemaVersion=1, scope='private-3d-qualification', releasedProfile='webgpu-2d-v1', status='failed', cases=[], stage=str(stage))
    cached = {} if args.fresh else cached_projects(args.cached_report)
    try:
        from web_publish import publish
        packages = []
        for language, name in [('csharp', 'AgentPipelineCSharp'), ('rust', 'AgentPipelineRust')]:
            source = ROOT / 'projects' / name
            project = cached.get(language, stage / (language + '-source'))
            # Only private publisher clones are ever changed, including reused caches.
            if not project.resolve().is_relative_to((ROOT / '.temp').resolve()):
                raise ValueError('Cached project is not a private .temp clone: ' + str(project))
            if not project.exists():
                shutil.copytree(source, project, ignore=shutil.ignore_patterns('Builds', 'target', 'bin', 'obj', '.vs'))
            if language == 'csharp':
                shutil.copy2(source / 'scripts/ModelProbe.cs', project / 'scripts/ModelProbe.cs')
                csproj = project / 'scripts/AetherGame.csproj'
                text = csproj.read_text(encoding='utf-8-sig')
                text = re.sub(r'(<ProjectReference\s+Include=")[^"]+AetherCore\.csproj"', lambda match: match[1] + (ROOT / 'managed/AetherCore/AetherCore.csproj').as_posix() + '"', text)
                csproj.write_text(text, encoding='utf-8')
            else:
                shutil.copy2(source / 'scripts/src/lib.rs', project / 'scripts/src/lib.rs')
                manifest = project / 'scripts/Cargo.toml'
                text = manifest.read_text(encoding='utf-8-sig')
                text = re.sub(r'aethercore\s*=\s*\{\s*path\s*=\s*"[^"]+"\s*\}', 'aethercore = { path = "' + (ROOT / 'rust/aethercore').as_posix() + '" }', text)
                manifest.write_text(text, encoding='utf-8')
            log_dir = args.report.parent / language
            result = publish(dict(project=str(project), language=language, operation='publish', target='web', timeoutMs=900000), log_dir)
            (log_dir / 'publish.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
            if result['status'] != 'passed':
                raise RuntimeError(language + ' verified 2D publishing failed: ' + str(log_dir / 'publish.json'))
            original = next(Path(item['path']).parent for item in result['artifacts'] if Path(item['path']).name == 'manifest.json')
            untouched = {name: digest(original / name) for name in ('manifest.json', 'game.pak', 'project.js')}
            private = stage / language
            shutil.copytree(original, private)
            # The original manifest is deliberately preserved byte-for-byte; it is
            # historical provenance, not an integrity claim for this private stage.
            inputs = stage / (language + '-assets')
            shutil.copytree(ROOT / 'resources/web/reference/assets/models', inputs / 'assets/models')
            shutil.copytree(ROOT / 'resources/web/reference/assets/materials', inputs / 'assets/materials')
            scene = (ROOT / 'resources/web/reference/scenes/BrowserModels.scene.toml').read_text(encoding='utf-8-sig')
            scene = scene.replace("[[entities]]\nname = 'Far primitive'", "[[entities.scripts]]\ntype = 'ModelProbe'\n\n[[entities]]\nname = 'Far primitive'")
            if "type = 'ModelProbe'" not in scene:
                raise RuntimeError('BrowserModels fixture no longer has the expected primitive boundary')
            (inputs / 'scenes').mkdir()
            (inputs / 'scenes/BrowserModels.scene.toml').write_text(scene, encoding='utf-8')
            with (log_dir / 'private-cook.log').open('wb') as log:
                subprocess.run([str(args.packer.resolve()), '--target', 'webgpu-2d-v1', str(inputs), str(private / 'game.pak')], stdout=log, stderr=subprocess.STDOUT, timeout=120, check=True)
            config_text = (private / 'project.js').read_text(encoding='utf-8-sig').strip()
            config = json.loads(config_text.removeprefix('globalThis.aetherProject=').removesuffix(';'))
            config.update(startupScene='BrowserModels', scenes=['BrowserModels'], qualification='private-3d-qualification', instructions='Private authored 3D language qualification. Select Play.')
            (private / 'project.js').write_text('globalThis.aetherProject=' + json.dumps(config) + ';\n', encoding='utf-8')
            (private / 'PRIVATE-3D-QUALIFICATION.txt').write_text('Private qualification only. The copied production manifest describes the unmodified verified 2D package, not this modified stage.\n', encoding='utf-8')
            assert all(digest(original / name) == value for name, value in untouched.items()), 'Original publish output changed during private staging'
            packages.append(dict(language=language, root=str(private), verifiedSource=str(original), originalHashes=untouched))
        config_path = stage / 'packages.json'
        config_path.write_text(json.dumps(packages, indent=2), encoding='utf-8')
        browser_report = args.report.parent / 'browser.json'
        command = ['node', str(ROOT / 'tools/web-runtime/authored-language-checks.mjs'), '--packages=' + str(config_path), '--report=' + str(browser_report), '--playwright=' + args.playwright]
        if args.executable:
            command.append('--executable=' + args.executable)
        browser_exit = run_browser(command, args.report.parent / 'browser.log')
        evidence = json.loads(browser_report.read_text(encoding='utf-8'))
        report.update(status=evidence['status'], cases=evidence['cases'], browserReport=str(browser_report), packages=packages)
        for package in packages:
            original = Path(package['verifiedSource'])
            if any(digest(original / name) != value for name, value in package['originalHashes'].items()):
                raise RuntimeError('Original verified package was modified: ' + str(original))
        report['originalPackagesUnchanged'] = True
        if browser_exit != 0:
            raise RuntimeError('Private browser qualification failed; inspect ' + str(browser_report))
    except Exception as error:
        report['error'] = str(error)
    args.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(status=report['status'], scope=report['scope'], report=str(args.report))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
