"""Exercise real CMake cache locks/staging with a controlled compiler process."""
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/project'))
from gameplay_jobs import _OwnedProcess

COMPILER = '''import json, os, pathlib, sys, time
args=sys.argv[1:]
cache=pathlib.Path(args[args.index('--target-dir')+1] if '--target-dir' in args else args[args.index('--artifacts-path')+1])
marker=os.environ['CACHE_TEST_MARKER']
pathlib.Path(os.environ['CACHE_TEST_STARTED']).touch()
time.sleep(float(os.environ.get('CACHE_TEST_SLEEP','0')))
if os.environ.get('CACHE_TEST_FAIL'): sys.exit(7)
out=cache/'debug/aether_gameplay.dll' if '--target-dir' in args else cache/'bin/AetherGame/debug/AetherGame.dll'
if os.environ.get('CACHE_TEST_RENAMED'): out=out.with_name('Other.dll')
out.parent.mkdir(parents=True,exist_ok=True);out.write_text(marker)
print(json.dumps({'reason':'compiler-artifact','filenames':[str(out)]} if '--target-dir' in args else {'Properties':{'TargetPath':str(out),'AssemblyName':out.stem}}))
'''


class BuildCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='aether build cache café & ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        # The helpers invoke '<compiler> build ...'. Python executes this extensionless
        # script from the test cwd, avoiding shell wrappers and their path quoting.
        compiler = self.root / 'build'
        compiler.write_text(COMPILER, encoding='utf-8')
        self.tool = Path(sys.executable)
        (self.root / 'Cargo.toml').touch()
        (self.root / 'AetherGame.csproj').touch()

    def launch(self, kind, label, **settings):
        env = dict(os.environ, CACHE_TEST_MARKER=label, CACHE_TEST_STARTED=str(self.root / (label + '.started')),
            **{'CACHE_TEST_' + key.upper(): str(value) for key, value in settings.items()})
        cache = self.root / (kind + '-cache')
        output = self.root / label
        common = [shutil.which('cmake'), '-DAETHER_LOCK_TIMEOUT=5']
        if kind == 'rust':
            args = ['-DAETHER_CARGO_EXECUTABLE=' + str(self.tool), '-DAETHER_RUSTC_EXECUTABLE=' + sys.executable,
                '-DAETHER_GAMEPLAY_MANIFEST=' + str(self.root / 'Cargo.toml'), '-DAETHER_RUST_TARGET_DIR=' + str(cache),
                '-DAETHER_RUST_OUTPUT_DIR=' + str(output), '-DAETHER_RUST_PROFILE=debug', '-DAETHER_RUST_MODULE=aether_gameplay.dll']
            script = ROOT / 'CMake/BuildRustGameplay.cmake'
        else:
            args = ['-DAETHER_DOTNET_EXECUTABLE=' + str(self.tool), '-DAETHER_GAMEPLAY_PROJECT=' + str(self.root / 'AetherGame.csproj'),
                '-DAETHER_MANAGED_ARTIFACTS=' + str(cache), '-DAETHER_MANAGED_OUTPUT=' + str(output)]
            script = ROOT / 'CMake/ManagedGameplayDriver/Build.cmake'
        log = (self.root / (label + '.log')).open('wb')
        self.addCleanup(log.close)
        # _OwnedProcess inherits this environment at process creation only.
        original = dict(os.environ)
        try:
            os.environ.update(env)
            child = _OwnedProcess(common + args + ['-P', str(script)], self.root, log)
        finally:
            os.environ.clear(); os.environ.update(original)
        self.addCleanup(child.close)
        child.test_log = self.root / (label + '.log')
        return child, output / ('aether_gameplay.dll' if kind == 'rust' else 'AetherGame.dll')

    def wait(self, child):
        deadline = time.monotonic() + 10
        while child.poll() is None and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertIsNotNone(child.poll(), 'owned build did not finish')
        return child.poll()

    def started(self, label):
        deadline = time.monotonic() + 5
        while not (self.root / (label + '.started')).exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue((self.root / (label + '.started')).exists(), 'compiler did not start')

    def test_failed_compile_never_stages_cached_success(self):
        for kind in ('rust', 'managed'):
            with self.subTest(kind=kind):
                first, output = self.launch(kind, kind + '-good')
                self.assertEqual(self.wait(first), 0)
                expected = output.read_text()
                failed, target = self.launch(kind, kind + '-failed', fail=1)
                self.assertNotEqual(self.wait(failed), 0)
                self.assertFalse(target.exists())
                self.assertEqual(output.read_text(), expected)

    def test_concurrent_builds_stage_their_own_output(self):
        for kind in ('rust', 'managed'):
            with self.subTest(kind=kind):
                first, output = self.launch(kind, kind + '-one', sleep=.8)
                self.started(kind + '-one')
                second, other = self.launch(kind, kind + '-two')
                time.sleep(.2)
                self.assertFalse((self.root / (kind + '-two.started')).exists())
                self.assertEqual(self.wait(first), 0)
                self.assertEqual(self.wait(second), 0)
                self.assertEqual(output.read_text(), kind + '-one')
                self.assertEqual(other.read_text(), kind + '-two')

    def test_success_with_changed_artifact_never_stages_old_cached_binary(self):
        for kind in ('rust', 'managed'):
            with self.subTest(kind=kind):
                first, output = self.launch(kind, kind + '-old-name')
                self.assertEqual(self.wait(first), 0)
                second, renamed = self.launch(kind, kind + '-new-name', renamed=1)
                self.assertNotEqual(self.wait(second), 0)
                self.assertFalse(renamed.exists())
                self.assertEqual(output.read_text(), kind + '-old-name')

    def test_cancelled_build_releases_cache_for_next_job(self):
        for kind in ('rust', 'managed'):
            with self.subTest(kind=kind):
                first, _ = self.launch(kind, kind + '-cancel', sleep=20)
                self.started(kind + '-cancel')
                first.kill(); first.close()
                second, output = self.launch(kind, kind + '-after')
                self.assertEqual(self.wait(second), 0)
                self.assertEqual(output.read_text(), kind + '-after')


if __name__ == '__main__':
    unittest.main()
