"""Native GPU regressions for dependency admission and required-font eviction.

Run manually after building Editor and aether-ctl. Each case owns an isolated
project below .temp and its own Editor process; a GPU/display is required.
Example: python scripts/test_streaming_admission.py --build-dir build/vs2026-msvc --config Debug
"""

import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import uuid
import zlib


REPO = Path(__file__).resolve().parents[1]
LOADED, ACTIVE, BLOCKED = 4, 5, 10


def png(width, height):
    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))

    pixels = (b"\0" + b"\x80\xa0\xc0\xff" * width) * height
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def fixtures(project, case):
    assets = project / "assets"
    scenes = project / "scenes"
    assets.mkdir(parents=True)
    scenes.mkdir()
    (project / "ProjectSettings.toml").write_text(
        "[project]\nname = 'Streaming admission regression'\nversion = 1\n"
        "[app]\nstartupscene = 'Admission'\n[paths]\nassets = 'assets'\nscenes = 'scenes'\n"
        "[window]\nwidth = 640\nheight = 480\n", encoding="utf-8")
    header = "[scene]\nname = 'Regression'\nkind = '2d'\nversion = 18\n"
    if case == "chain":
        regions = ["chain"]
        # An empty map still has the real map -> tileset -> atlas dependency chain.
        reference = b"project://assets/chain.tileset.toml"
        (assets / "chain.tilemap").write_bytes(
            b"ATLM" + struct.pack("<II", 2, len(reference)) + reference + struct.pack("<fII", 1, 0, 0))
        (assets / "chain.tileset.toml").write_text(
            "[tileset]\nschema_version = 1\nname = 'Chain'\ncell_size = 1\ntile_count = 1\n"
            "[tile.0]\nid = '0000000000000001'\nname = 'Tile'\n"
            "atlas = 'project://assets/chain.spriteatlas.toml'\nsprite_id = '0000000000000001'\n", encoding="utf-8")
        (assets / "chain.spriteatlas.toml").write_text(
            "[atlas]\nschema_version = 2\ntexture = 'project://assets/tiny.texture'\n"
            "texture_width = 1\ntexture_height = 1\nsprite_count = 1\n"
            "[sprite.0]\nid = '0000000000000001'\nname = 'Tile'\nx = 0\ny = 0\nwidth = 1\nheight = 1\n", encoding="utf-8")
        (assets / "tiny.texture").write_bytes(png(1, 1))
        (scenes / "chain.scene.toml").write_text(
            header + "[[entities]]\nname = 'Map'\n[entities.tile_map]\ntilemap = 'project://assets/chain.tilemap'\n", encoding="utf-8")
    else:
        regions = ["texture", "font"]
        # Texture::Decode accepts image headers independently of the extension.
        # A cooked extension prevents the editor importer from replacing this
        # controlled RGBA allocation with a compressed texture during the test.
        (assets / "pressure.texture").write_bytes(png(600, 600))
        # Preserve all glyphs/curve indices and pad unused rows. The resulting
        # valid font has an allocation large enough to create pressure even on
        # devices whose image-allocation alignment differs.
        font = bytearray((REPO / "resources/fonts/Roboto-Regular.fontcurves").read_bytes())
        old_count = struct.unpack_from("<I", font, 12)[0]
        texels = 1024 * 512
        struct.pack_into("<III", font, 12, texels, 1024, 512)
        font.extend(bytes((texels - old_count) * 16))
        (assets / "required.fontcurves").write_bytes(font)
        (scenes / "texture.scene.toml").write_text(
            header + "[[entities]]\nname = 'Prefetched sprite'\n[entities.sprite]\ntexture = 'project://assets/pressure.texture'\n", encoding="utf-8")
        (scenes / "font.scene.toml").write_text(
            header + "[[entities]]\nname = 'Required text'\n[entities.ui_text]\ntext = 'Regression'\nfont = 'project://assets/required.fontcurves'\n", encoding="utf-8")
    root = ""
    for region in regions:
        root += (f"[[stream_regions]]\nid = '{region}'\nscene = 'project://scenes/{region}.scene.toml'\n"
                 "bounds_min = [10000, 10000, 10000]\nbounds_max = [10001, 10001, 10001]\nalways_loaded = false\n")
    (scenes / "Admission.scene.toml").write_text(root + header, encoding="utf-8")


class NativeEditor:
    def __init__(self, args, case):
        self.args = args
        # Refuse to contact an unrelated Editor already using the requested port.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            if os.name == "nt":
                reservation.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            reservation.bind(("127.0.0.1", args.port))
        self.project = REPO / ".temp" / f"streaming-admission-{case}-{uuid.uuid4().hex}"
        fixtures(self.project, case)
        logs = REPO / "logs"
        logs.mkdir(exist_ok=True)
        self.log_path = logs / f"streaming-admission-{case}-{self.project.name[-32:]}.log"
        self.log = self.log_path.open("w", encoding="utf-8")
        environment = dict(os.environ, AETHER_CONTROL_PORT=str(args.port))
        options = {}
        if os.name == "nt":
            startup = subprocess.STARTUPINFO()
            startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow = 0
            options.update(startupinfo=startup, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        else:
            options["start_new_session"] = True
        self.process = subprocess.Popen(
            [str(args.editor), "--project", str(self.project)], cwd=args.build_dir,
            env=environment, stdout=self.log, stderr=subprocess.STDOUT, **options)

    def call(self, method, **params):
        if self.process.poll() is not None:
            raise RuntimeError(f"Owned Editor exited with {self.process.returncode}; see {self.log_path}")
        result = subprocess.run([str(self.args.ctl), "--port", str(self.args.port), method, "-"],
                                input=json.dumps(params), text=True, capture_output=True, timeout=40)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        value = json.loads(result.stdout)
        if "error" in value:
            raise RuntimeError(f"{method}: {value['error']}")
        return value

    def wait(self, predicate, description):
        deadline = time.monotonic() + self.args.timeout
        last = None
        while time.monotonic() < deadline:
            last = self.call("streaming.status")
            if predicate(last):
                return last
            time.sleep(0.2)
        raise AssertionError(f"{description}\nLast native scheduler state:\n{json.dumps(last, indent=2)}")

    def ready(self):
        deadline = time.monotonic() + self.args.timeout
        while True:
            try:
                self.call("info")
                break
            except RuntimeError:
                if self.process.poll() is not None or time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        for key, value in {"defaultCameraSource": "false", "cpuBudgetMiB": "256", "gpuBudgetMiB": "64",
                           "stagingBudgetMiB": "32", "uploadBudgetKiBPerFrame": "4096",
                           "maxPendingRequests": "1" if self.args.case == "chain" else "128"}.items():
            self.call("settings.set", key=f"streaming.{key}", value=value)
        self.wait(lambda status: {row["id"] for row in status["regions"]} ==
                  ({"chain"} if self.args.case == "chain" else {"texture", "font"}), "Fixture regions were not configured")

    def close(self):
        # Only the process launched above belongs to this test.
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        self.log.close()


def region_state(status, name):
    return next(row["state"] for row in status["regions"] if row["id"] == name)


def asset_rows(status):
    return status.get("backend", {}).get("assets", {}).get("assets", [])


def run_case(args, case):
    args.case = case
    editor = NativeEditor(args, case)
    try:
        editor.ready()
        if case == "chain":
            editor.call("streaming.request", region="chain", activate=False)
            result = editor.wait(lambda status: region_state(status, "chain") == LOADED,
                                 "Tilemap dependency chain must load with maxPendingRequests=1")
            rows = asset_rows(result)
            assert len(rows) == 3 and all(row["state"] == LOADED for row in rows), rows
        else:
            # Measure native allocations first; this avoids hard-coding a VMA
            # allocation requirement that varies by GPU/driver.
            font_claim = editor.call("streaming.request", region="font", activate=False)["id"]
            result = editor.wait(lambda status: region_state(status, "font") == LOADED,
                                 "Font must fit the GPU budget alone")
            font_bytes = next(row["gpuBytes"] for row in asset_rows(result) if row["type"] == "font")
            assert font_bytes, result
            editor.call("streaming.release", id=font_claim)
            editor.wait(lambda status: not asset_rows(status) and
                        sum(status["backend"]["assets"][key] for key in
                            ("gpuResidentBytes", "gpuPendingBytes", "gpuRetiringBytes")) == 0,
                        "Font calibration resources must retire before applying pressure")
            editor.call("streaming.request", region="texture", activate=False)
            result = editor.wait(lambda status: region_state(status, "texture") == LOADED,
                                 "Nonrequired texture must fit the GPU budget alone")
            texture = next(row for row in asset_rows(result) if row["reference"].endswith("pressure.texture"))
            assert not texture["required"] and texture["claims"] == 1, texture
            mib = 1024 * 1024
            budget_mib = (max(texture["gpuBytes"], font_bytes) + mib - 1) // mib
            assert texture["gpuBytes"] + font_bytes > budget_mib * mib, "Fixture allocations do not create GPU budget pressure"
            editor.call("settings.set", key="streaming.gpuBudgetMiB", value=str(budget_mib))
            editor.call("streaming.request", region="font", activate=True)
            result = editor.wait(lambda status: region_state(status, "font") == ACTIVE,
                                 "Required font must evict the nonrequired texture and activate")
            texture = next(row for row in asset_rows(result) if row["reference"].endswith("pressure.texture"))
            assert texture["state"] == BLOCKED and "evicted" in texture["reason"], texture
        print(f"PASS: {case}; native scheduler assertions passed; project={editor.project}")
    finally:
        editor.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO / "build/vs2026-msvc")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--ctl", type=Path, help="Existing aether-ctl executable (its build configuration may differ)")
    parser.add_argument("--case", choices=("all", "chain", "font"), default="all")
    parser.add_argument("--port", type=int, default=18787)
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    suffix = ".exe" if os.name == "nt" else ""
    args.editor = args.build_dir / "src/app" / args.config / f"Editor{suffix}"
    args.ctl = args.ctl.resolve() if args.ctl else args.build_dir / "tools/control-client" / args.config / f"aether-ctl{suffix}"
    if not args.editor.is_file() or not args.ctl.is_file():
        parser.error(f"Build Editor and aether-ctl first; expected {args.editor} and {args.ctl}")
    cases = ("chain", "font") if args.case == "all" else (args.case,)
    failures = []
    for case in cases:
        try:
            run_case(args, case)
        except (AssertionError, RuntimeError, OSError, subprocess.TimeoutExpired) as error:
            failures.append(case)
            print(f"FAIL: {case}: {error}", flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
