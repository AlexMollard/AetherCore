"""Desktop Cargo driver contract checks; no GPU or .NET host needed."""
import pathlib, shutil, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]
class RustBuildDriverTests(unittest.TestCase):
    def test_missing_manifest_fails_before_cargo(self):
        with tempfile.TemporaryDirectory(prefix="aether rust driver ") as d:
            p = subprocess.run(["cmake", "-S", str(ROOT / "CMake/RustGameplayDriver"), "-B", str(pathlib.Path(d)/"build"), "-DAETHER_GAMEPLAY_MANIFEST="+str(pathlib.Path(d)/"missing/Cargo.toml"), "-DAETHER_RUST_TARGET_DIR="+str(pathlib.Path(d)/"target"), "-DAETHER_RUST_OUTPUT_DIR="+str(pathlib.Path(d)/"out")], capture_output=True, text=True)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn("Gameplay Cargo manifest does not exist", p.stdout+p.stderr)
if __name__ == "__main__": unittest.main()
