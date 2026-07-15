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
    build dir cached at startup can't stick. Prefers AETHER_CTL, then the first
    candidate build dir / config that actually contains the exe; otherwise returns the
    primary Debug path so the 'build it' error names a sensible target."""
    env = os.environ.get("AETHER_CTL")
    if env:
        return env
    exe = "aether-ctl.exe" if os.name == "nt" else "aether-ctl"
    build_env = os.environ.get("AETHER_BUILD_DIR")
    names = ([build_env] if build_env else []) + list(_BUILD_DIR_NAMES)
    fallback: Path | None = None
    for name in names:
        base = Path(name) if os.path.isabs(name) else (REPO / name)
        for cfg in ("RelWithDebInfo", "Debug", "Release", None):
            cand = base / "tools" / "control-client" / cfg / exe if cfg else base / "tools" / "control-client" / exe
            fallback = fallback or cand
            if cand.exists():
                return str(cand)
    return str(fallback)


GAUNTLET = REPO / "scripts" / "Run-DebugGauntlet.ps1"


# ── Subprocess bridges ───────────────────────────────────────────────────────

def _ctl(method: str, params: dict | None = None) -> dict:
    """Invoke aether-ctl for one control-endpoint method; return a result dict
    ({"result": ...} on success, {"error": ...} on failure)."""
    ctl = _ctl_path()
    if not Path(ctl).exists():
        return {"error": f"aether-ctl not found (searched build dirs under {REPO}); build it: cmake --build {BUILD_DIR} --target aether-ctl"}
    args = [ctl, "--port", str(PORT), method]
    if params:
        args.append(json.dumps(params))
    try:
        # Generous: engine.play rebuilds C# scripts before replying.
        proc = subprocess.run(args, capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": f"aether-ctl failed to run: {exc}"}
    if proc.returncode != 0:
        return {"error": (proc.stderr or proc.stdout or "aether-ctl error").strip()}
    out = proc.stdout.strip()
    try:
        return {"result": json.loads(out) if out else {}}
    except json.JSONDecodeError:
        return {"result": out}


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
    return [RUN_GAUNTLET] + _engine_tools()


def _call_tool(name: str, arguments: dict) -> dict:
    if name == "run_gauntlet":
        return _run_gauntlet(arguments.get("mode", "ci"), arguments.get("config", "Debug"))
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
