#!/usr/bin/env python3
"""AetherCore MCP server.

A dependency-free (stdlib-only) Model Context Protocol server over stdio.

  * run_gauntlet - runs scripts/Run-DebugGauntlet.ps1 and returns its report
                   (build + unit + optional GPU validation smokes). No running
                   engine required. This is the one static tool.
  * everything else - GENERATED from the editor control endpoint's own "describe"
                   manifest. The endpoint (src/app/editor/ControlMethods.cpp) is
                   the single source of truth; adding a capability there makes the
                   MCP tool appear automatically, with zero changes here. The
                   manifest is merged into manifest.json next to this file so
                   Launcher and Editor tools stay visible across handoff and
                   even when neither endpoint is running.

ENet is a C library with no maintained Python binding, so it lives entirely on
the C++ side; this server only spawns subprocesses (aether-ctl + PowerShell).

Path/port resolution (all overridable by environment):
  AETHER_REPO          repo root (default: two levels above this file)
  AETHER_BUILD_DIR     CMake build dir (default: first existing under the repo)
  AETHER_CTL           path to aether-ctl(.exe) (default: found under the build dir)
  AETHER_CONTROL_PORT  editor control port (default: 8787)
  AETHER_PWSH          PowerShell executable for the gauntlet (default: pwsh)
"""

import json
import os
import subprocess
import sys
from pathlib import Path

# ── Configuration ────────────────────────────────────────────────────────────

REPO = Path(os.environ.get("AETHER_REPO", Path(__file__).resolve().parents[2]))
PORT = os.environ.get("AETHER_CONTROL_PORT", "8787")
PWSH = os.environ.get("AETHER_PWSH", "pwsh")
MANIFEST_CACHE = Path(__file__).resolve().parent / "manifest.json"


# Candidate build dirs, tried in order. Includes the legacy hyphenated name so an
# older local layout still resolves. `build/vs2022-msvc` is the current default.
_BUILD_DIR_NAMES = ("build/vs2022-msvc", "build-vs2022-msvc", "build/default", "build", "build/ninja-clang")


def _find_build_dir() -> Path:
    env = os.environ.get("AETHER_BUILD_DIR")
    if env:
        return Path(env)
    for name in _BUILD_DIR_NAMES:
        candidate = REPO / name
        if candidate.exists():
            return candidate
    return REPO / "build/vs2022-msvc"


BUILD_DIR = _find_build_dir()


def _ctl_path() -> str:
    """Locate aether-ctl. Resolved LAZILY (per call, not at import) so a client built
    *after* the server started is picked up without restarting the MCP, and so a wrong
    build dir cached at startup can't stick. Prefers AETHER_CTL, then the NEWEST
    (by mtime) exe across every candidate build dir / config: with several build
    trees on disk, first-found returned stale binaries missing newer CLI features
    (e.g. the '-' stdin params mode -> "params is not valid JSON: -"). Falls back
    to the primary Debug path so the 'build it' error names a sensible target."""
    env = os.environ.get("AETHER_CTL")
    if env:
        return env
    exe = "aether-ctl.exe" if os.name == "nt" else "aether-ctl"
    build_env = os.environ.get("AETHER_BUILD_DIR")
    names = ([build_env] if build_env else []) + list(_BUILD_DIR_NAMES)
    fallback: Path | None = None
    existing: list[Path] = []
    for name in names:
        base = Path(name) if os.path.isabs(name) else (REPO / name)
        for cfg in ("RelWithDebInfo", "Debug", "Release", None):
            cand = base / "tools" / "control-client" / cfg / exe if cfg else base / "tools" / "control-client" / exe
            fallback = fallback or cand
            if cand.exists():
                existing.append(cand)
    if existing:
        return str(max(existing, key=lambda p: p.stat().st_mtime))
    return str(fallback)


def _assetpacker_path() -> str:
    """Locate AssetPacker. Same lazy/newest-mtime resolution as _ctl_path(): resolved
    per call (not at import) so a client built after the server started is picked up
    without restarting the MCP, preferring the NEWEST (by mtime) exe across every
    candidate build dir / config. AssetPacker's own RUNTIME_OUTPUT_DIRECTORY is
    <buildDir>/tools directly - one level up from aether-ctl's tools/control-client -
    and single-config generators (e.g. the clangd/Ninja preset) put the exe straight
    there with no <Config> subfolder at all, so that bare path is tried too."""
    exe = "AssetPacker.exe" if os.name == "nt" else "AssetPacker"
    build_env = os.environ.get("AETHER_BUILD_DIR")
    names = ([build_env] if build_env else []) + list(_BUILD_DIR_NAMES)
    fallback: Path | None = None
    existing: list[Path] = []
    for name in names:
        base = Path(name) if os.path.isabs(name) else (REPO / name)
        for cfg in ("RelWithDebInfo", "Debug", "Release", None):
            cand = base / "tools" / cfg / exe if cfg else base / "tools" / exe
            fallback = fallback or cand
            if cand.exists():
                existing.append(cand)
    if existing:
        return str(max(existing, key=lambda p: p.stat().st_mtime))
    return str(fallback)


GAUNTLET = REPO / "scripts" / "Run-DebugGauntlet.ps1"
KENNEY_MANIFEST = REPO / "tools" / "assetpack" / "kenney_packs.toml"
KENNEY_CACHE_DIR = REPO / ".temp" / "kenney-cache"


# ── Subprocess bridges ───────────────────────────────────────────────────────

def _ctl(method: str, params: dict | None = None) -> dict:
    """Invoke aether-ctl for one control-endpoint method; return a result dict
    ({"result": ...} on success, {"error": ...} on failure)."""
    ctl = _ctl_path()
    if not Path(ctl).exists():
        return {"error": f"aether-ctl not found (searched build dirs under {REPO}); build it: cmake --build {BUILD_DIR} --target aether-ctl"}
    args = [ctl, "--port", str(PORT), method]
    input_json = None
    if params:
        # Avoid Windows' ~32 KiB command-line limit. Batch scene requests can
        # legitimately be hundreds of KiB, so stream JSON through stdin.
        args.append("-")
        input_json = json.dumps(params)
    try:
        # Generous: engine.play rebuilds C# scripts before replying.
        proc = subprocess.run(args, input=input_json, capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": f"aether-ctl failed to run: {exc}"}
    if proc.returncode != 0:
        return {"error": (proc.stderr or proc.stdout or "aether-ctl error").strip()}
    out = proc.stdout.strip()
    try:
        return {"result": json.loads(out) if out else {}}
    except json.JSONDecodeError:
        return {"result": out}


def _kenney(args: list[str]) -> dict:
    """Invoke `AssetPacker kenney <args>`; return a result dict ({"result": ...} on
    success, {"error": ...} on failure). Every invocation prints exactly one line of
    JSON to stdout regardless of exit code, so it's parsed either way rather than
    gated on the return code (mirrors AssetPacker's kenney CLI contract)."""
    assetpacker = _assetpacker_path()
    if not Path(assetpacker).exists():
        return {"error": f"AssetPacker not found (searched build dirs under {REPO}); build it: cmake --build {BUILD_DIR} --target AssetPacker"}
    try:
        # Generous: a cold pack download can take a few seconds.
        proc = subprocess.run([assetpacker, "kenney", *args], capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": f"AssetPacker failed to run: {exc}"}
    try:
        parsed = json.loads(proc.stdout.strip())
    except json.JSONDecodeError:
        return {"error": (proc.stderr or proc.stdout or "AssetPacker kenney failed").strip()}
    if parsed.get("ok"):
        return {"result": parsed}
    return {"error": parsed.get("error", proc.stderr.strip() or "AssetPacker kenney failed")}


def _run_gauntlet(mode: str, config: str) -> dict:
    if not GAUNTLET.exists():
        return {"error": f"gauntlet not found at {GAUNTLET}"}
    args = [PWSH, "-NoProfile", "-File", str(GAUNTLET), "-BuildDir", str(BUILD_DIR), "-Config", config]
    if mode == "ci":
        args.append("-CI")
    try:
        subprocess.run(args, capture_output=True, text=True, timeout=1200)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": f"gauntlet failed to run: {exc}"}
    report = BUILD_DIR / "gauntlet-report.json"
    if not report.exists():
        return {"error": "gauntlet produced no report.json"}
    try:
        return {"result": json.loads(report.read_text(encoding="utf-8"))}
    except (OSError, json.JSONDecodeError) as exc:
        return {"error": f"could not read report: {exc}"}


# ── Tools ────────────────────────────────────────────────────────────────────

RUN_GAUNTLET = {
    "name": "run_gauntlet",
    "description": "Build Editor/GameRuntime/EngineTests, run the unit suite, and (in full mode, needs a GPU) smoke-run the editor + runtime under Vulkan validation. Returns the structured pass/fail report. Does not need the editor running.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "mode": {"type": "string", "enum": ["full", "ci"], "default": "ci", "description": "'ci' = build + unit only; 'full' = also GPU validation smokes."},
            "config": {"type": "string", "enum": ["Debug", "Release"], "default": "Debug"},
        },
    },
}


KENNEY_LIST_PACKS = {
    "name": "kenney_list_packs",
    "description": "List the Kenney CC0 asset packs available for import, from the checked-in tools/assetpack/kenney_packs.toml manifest. Does not need the editor running.",
    "inputSchema": {
        "type": "object",
        "properties": {},
    },
}

KENNEY_LIST_MODELS = {
    "name": "kenney_list_models",
    "description": "List the .glb/.gltf models directly under one Kenney pack's model directory, by slug (from kenney_list_packs). The first call for a pack downloads and caches its zip (a few seconds); every later call for the same pack/version is instant. Does not need the editor running.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "slug": {"type": "string", "description": "Pack slug from kenney_list_packs, e.g. 'factory-kit'."},
        },
        "required": ["slug"],
    },
}

KENNEY_IMPORT = {
    "name": "kenney_import",
    "description": "Import one CC0 Kenney model into a project as a spawnable prop: fetches/caches the pack, bakes the model, appends a CREDITS.md attribution line, and registers the prop in PropSpawner.cs (a bare model reference - the engine builds its collider as a convex hull from the baked mesh itself at spawn time, so there is no shape to pick here) - all in one call. Idempotent: importing the same model twice reports it already present instead of duplicating anything. Does not need the editor running.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "slug": {"type": "string", "description": "Pack slug from kenney_list_packs, e.g. 'factory-kit'."},
            "zipMemberPath": {"type": "string", "description": "Model path inside the pack zip, from kenney_list_models."},
            "project": {"type": "string", "description": "Folder name under projects/, e.g. 'Sandbox'."},
            "category": {"type": "string", "description": "Prop category / models subfolder, e.g. 'Props'."},
            "propName": {"type": "string", "description": "PascalCase file stem for the imported prop."},
            "displayName": {"type": "string", "description": "Human-readable name shown in the prop spawner."},
            "mass": {"type": "number", "description": "Rigid-body mass in kg."},
            "registerInCatalog": {
                "type": "boolean",
                "default": True,
                "description": "Whether to add a PropSpawner.cs catalogue entry. False for viewmodel-style imports that must never appear in the spawn menu.",
            },
        },
        "required": ["slug", "zipMemberPath", "project", "category", "propName", "displayName", "mass"],
    },
}


KENNEY_IMPORT_PACK = {
    "name": "kenney_import_pack",
    "description": "Bulk-import every Kenney model in a pack matching a filter (empty filter = the whole pack) as spawnable props - the same per-item fetch/cache/bake/credit/catalogue operation as kenney_import, run once per matching model, so one bad model never poisons the rest of the batch (failures are reported by name and reason). Always check kenney_list_models first and pass an explicit filter: importing an entire multi-hundred-file pack unfiltered is rarely what you want. Idempotent: re-running the same filter reports everything as already-present instead of duplicating it. Does not need the editor running.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "slug": {"type": "string", "description": "Pack slug from kenney_list_packs, e.g. 'factory-kit'."},
            "project": {"type": "string", "description": "Folder name under projects/, e.g. 'Sandbox'."},
            "category": {"type": "string", "description": "Destination folder under assets/models/ for the whole run. Omit for the default: PascalCase(slug), e.g. 'FactoryKit'."},
            "filter": {"type": "string", "description": "Case-insensitive substring match on each model's file name; omit or empty to import every model in the pack (use deliberately, not as a default)."},
            "mass": {"type": "number", "default": 1.0, "description": "Rigid-body mass in kg, applied to every imported model."},
            "registerInCatalog": {
                "type": "boolean",
                "default": True,
                "description": "Whether to add a PropSpawner.cs catalogue entry for each imported model.",
            },
        },
        "required": ["slug", "project"],
    },
}


KENNEY_IMPORT_FONT = {
    "name": "kenney_import_font",
    "description": "Import a Kenney icon/text font into a project's assets/fonts/: fetches/caches the pack, extracts the .ttf (+ an optional glyph-name-to-codepoint reference text file), bakes it with the engine's glyph-outline font pipeline (the same one 'AssetPacker bake-font' and every hand-authored project font use), and appends a CREDITS.md line. A genuinely different pipeline from kenney_import - fonts are not models, get no collider, and are never registered in PropSpawner.cs (they are UI assets). Idempotent. Does not need the editor running.",
    "inputSchema": {
        "type": "object",
        "properties": {
            "slug": {"type": "string", "description": "Pack slug from kenney_list_packs, e.g. 'input-prompts'."},
            "ttfZipMemberPath": {"type": "string", "description": "Path to the .ttf/.otf inside the pack zip."},
            "charMapZipMemberPath": {"type": "string", "description": "Path to a glyph-name -> codepoint reference text file inside the pack zip, if the pack ships one. Omit to skip."},
            "project": {"type": "string", "description": "Folder name under projects/, e.g. 'Sandbox'."},
            "fontName": {"type": "string", "description": "Output stem: assets/fonts/<fontName>.ttf/.fontcurves, and the exact string a script passes to Ui.SetFont."},
        },
        "required": ["slug", "ttfZipMemberPath", "project", "fontName"],
    },
}


def _fetch_manifest() -> list:
    """Return the union of cached and live endpoint methods.

    Launcher and Editor expose different method sets on the same port. Keeping
    their union makes a fixed MCP client tool list usable before and after the
    handoff; calls still go to whichever endpoint currently owns the port.
    """
    cached = []
    if MANIFEST_CACHE.exists():
        try:
            cached = json.loads(MANIFEST_CACHE.read_text(encoding="utf-8")).get("methods", [])
        except (OSError, ValueError):
            pass

    result = _ctl("describe")
    methods = result["result"].get("methods") if isinstance(result.get("result"), dict) else None
    if methods is not None:
        merged = list(cached)
        index_by_tool = {method.get("tool"): index for index, method in enumerate(merged) if method.get("tool")}
        for method in methods:
            tool = method.get("tool")
            if tool in index_by_tool:
                merged[index_by_tool[tool]] = method
            else:
                index_by_tool[tool] = len(merged)
                merged.append(method)
        try:
            MANIFEST_CACHE.write_text(json.dumps({"methods": merged}, indent=2), encoding="utf-8")
        except OSError:
            pass
        return merged
    return cached


def _engine_tools() -> list:
    tools = []
    for m in _fetch_manifest():
        tools.append({
            "name": m["tool"],
            "wire": m["name"],
            "description": m["description"],
            "inputSchema": m.get("paramsSchema") or {"type": "object", "properties": {}},
        })
    return tools


def _list_tools() -> list:
    return [RUN_GAUNTLET, KENNEY_LIST_PACKS, KENNEY_LIST_MODELS, KENNEY_IMPORT, KENNEY_IMPORT_PACK, KENNEY_IMPORT_FONT] + _engine_tools()


def _call_tool(name: str, arguments: dict) -> dict:
    if name == "run_gauntlet":
        return _run_gauntlet(arguments.get("mode", "ci"), arguments.get("config", "Debug"))
    if name == "kenney_list_packs":
        return _kenney(["list-packs", str(KENNEY_MANIFEST)])
    if name == "kenney_list_models":
        return _kenney(["list-models", str(KENNEY_MANIFEST), arguments.get("slug", ""), str(KENNEY_CACHE_DIR)])
    if name == "kenney_import":
        project_root = REPO / "projects" / arguments.get("project", "")
        args = [
            "import", str(KENNEY_MANIFEST), arguments.get("slug", ""), arguments.get("zipMemberPath", ""),
            str(project_root), arguments.get("category", ""), arguments.get("propName", ""),
            arguments.get("displayName", ""), str(arguments.get("mass", 0)),
        ]
        if not arguments.get("registerInCatalog", True):
            args.append("--no-catalog")
        args.append(str(KENNEY_CACHE_DIR))
        return _kenney(args)
    if name == "kenney_import_pack":
        project_root = REPO / "projects" / arguments.get("project", "")
        args = ["import-pack", str(KENNEY_MANIFEST), arguments.get("slug", ""), str(project_root)]
        if arguments.get("category"):
            args.append(f"--category={arguments['category']}")
        if arguments.get("filter"):
            args.append(f"--filter={arguments['filter']}")
        if arguments.get("mass") is not None:
            args.append(f"--mass={arguments['mass']}")
        if not arguments.get("registerInCatalog", True):
            args.append("--no-catalog")
        args.append(str(KENNEY_CACHE_DIR))
        return _kenney(args)
    if name == "kenney_import_font":
        project_root = REPO / "projects" / arguments.get("project", "")
        return _kenney([
            "import-font", str(KENNEY_MANIFEST), arguments.get("slug", ""), arguments.get("ttfZipMemberPath", ""),
            arguments.get("charMapZipMemberPath") or "-", str(project_root), arguments.get("fontName", ""),
            str(KENNEY_CACHE_DIR),
        ])
    for tool in _engine_tools():
        if tool["name"] == name:
            return _ctl(tool["wire"], arguments)
    return {"error": f"unknown tool: {name}"}


# ── Minimal MCP (JSON-RPC 2.0 over newline-delimited stdio) ───────────────────

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "aethercore", "version": "0.2.0"}


def _result(msg_id, result):
    return {"jsonrpc": "2.0", "id": msg_id, "result": result}


def _error(msg_id, code, message):
    return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}}


def _handle(msg: dict):
    method = msg.get("method")
    msg_id = msg.get("id")
    params = msg.get("params") or {}

    if method == "initialize":
        return _result(msg_id, {
            "protocolVersion": params.get("protocolVersion", PROTOCOL_VERSION),
            "capabilities": {"tools": {}},
            "serverInfo": SERVER_INFO,
        })
    if method in ("notifications/initialized", "initialized"):
        return None
    if method == "ping":
        return _result(msg_id, {})
    if method == "tools/list":
        listed = [{"name": t["name"], "description": t["description"], "inputSchema": t["inputSchema"]} for t in _list_tools()]
        return _result(msg_id, {"tools": listed})
    if method == "tools/call":
        name = params.get("name")
        arguments = params.get("arguments") or {}
        try:
            out = _call_tool(name, arguments)
        except Exception as exc:  # noqa: BLE001 - surface any tool error as text
            out = {"error": f"tool raised: {exc}"}
        is_error = isinstance(out, dict) and "error" in out
        payload = out.get("result", out.get("error")) if isinstance(out, dict) else out
        text = json.dumps(payload, indent=2)
        return _result(msg_id, {"content": [{"type": "text", "text": text}], "isError": is_error})

    if msg_id is not None:
        return _error(msg_id, -32601, f"method not found: {method}")
    return None


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        response = _handle(msg)
        if response is not None:
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
