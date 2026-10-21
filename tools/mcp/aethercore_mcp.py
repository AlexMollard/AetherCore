#!/usr/bin/env python3
"""AetherCore MCP server.

A dependency-free (stdlib-only) Model Context Protocol server over stdio.

  * run_gauntlet - runs scripts/Run-DebugGauntlet.ps1 and returns its report
                   (build + unit + optional GPU validation smokes). No running
                   engine required.
  * gameplay_job_start/status/cancel - persistent project pipeline jobs, with no
                   editor endpoint required.
  * engine/launcher tools - GENERATED from the editor control endpoint's "describe"
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
  AETHER_PROJECT_TOOL  standalone aether-project worker (default: newest app bundle)
"""

import json
import os
import subprocess
import sys
import tempfile
import signal
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'project'))
import gameplay_jobs
from toolchain_environment import child_environment

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
    ({"result": ...} on success, {"error": ...} on failure). Shared gameplay
    report failures retain both keys so MCP error content keeps the full report."""
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
        proc = subprocess.run(args, input=input_json, capture_output=True, text=True, encoding="utf-8", timeout=45)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": f"aether-ctl failed to run: {exc}"}
    if proc.returncode != 0:
        try:
            payload = json.loads(proc.stdout)
        except json.JSONDecodeError:
            payload = None
        if isinstance(payload, dict) and payload.get("ok") is False:
            report = payload.get("report")
            if isinstance(report, dict) and type(report.get("schemaVersion")) is int and report["schemaVersion"] == 1:
                exit_code = report.get("exitCode")
                status = report.get("status")
                if isinstance(status, str) and type(exit_code) is int and exit_code == {"failed": 1, "cancelled": 130, "skipped": 3}.get(status):
                    message = payload.get("message")
                    return {"result": payload, "error": message if isinstance(message, str) and message else "Gameplay operation failed"}
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

def _agent_editor_helper(script: str, params: list[str], timeout: float) -> dict:
    log_root = REPO / 'logs/agent-editor/requests'
    log_root.mkdir(parents=True, exist_ok=True)
    helper_logs = Path(tempfile.mkdtemp(prefix='helper-', dir=log_root))
    try:
        # A detached GUI child can retain inherited pipe handles after PowerShell
        # exits. Files let us wait for the helper itself, without waiting for EOF
        # from an editor that is intentionally still running.
        with (helper_logs / 'stdout.log').open('wb') as output, (helper_logs / 'stderr.log').open('wb') as errors:
            process = subprocess.run([PWSH, '-NoProfile', '-File', str(REPO / 'scripts' / script), *params],
                                     stdout=output, stderr=errors, text=True, encoding='utf-8', timeout=timeout, cwd=str(REPO),
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0, env=child_environment())
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {'error': f'Pog Champ, owned editor helper failed: {exc}. Close any successfully launched editor with agent_editor_close using its receiptPath.'}
    try:
        stdout = (helper_logs / 'stdout.log').read_text(encoding='utf-8-sig')
        stderr = (helper_logs / 'stderr.log').read_text(encoding='utf-8-sig')
    except (OSError, UnicodeError) as exc:
        return {'error': f'Pog Champ, could not read owned editor helper JSON: {exc}. Logs: {helper_logs}. Use the launch receipt with agent_editor_close for any editor requiring cleanup.'}
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError:
        message = (stderr or stdout or 'Pog Champ, owned editor helper produced no JSON receipt.').strip()
        return {'error': message + '\nPog Champ, use the launch receipt with agent_editor_close for any editor that still requires cleanup.'}
    if not isinstance(payload, dict):
        return {'error': 'Pog Champ, owned editor helper returned an invalid JSON object.'}
    if '-Receipt' in params:
        payload.setdefault('receiptPath', params[params.index('-Receipt') + 1])
    payload.setdefault('cleanupInstructions', 'Pog Champ, close this owned editor with agent_editor_close using its receiptPath when finished.'
                       if payload.get('cleanupRequired', True) else 'Pog Champ, this owned editor is closed; no cleanup remains.')
    response = {'result': payload}
    if process.returncode != 0 or payload.get('ok') is False:
        response['error'] = payload.get('message') or (stderr.strip() if stderr else 'Pog Champ, owned editor operation failed.')
    return response


def _agent_editor(name: str, arguments: dict) -> dict:
    if name == 'agent_editor_launch':
        port = arguments.get('port')
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError('Pog Champ, port must be an integer from 1 through 65535.')
        timeout = arguments.get('timeoutSeconds', 90)
        params = ['-Project', arguments.get('project', ''), '-Name', arguments.get('name', ''), '-Port', str(port), '-TimeoutSeconds', str(timeout)]
        if arguments.get('buildDir'):
            params += ['-BuildDir', arguments['buildDir']]
        if arguments.get('background', True) is False:
            params.append('-Visible')
        return _agent_editor_helper('Start-AgentEditor.ps1', params, timeout + 25)
    receipt = arguments.get('receiptPath', arguments.get('receipt', ''))
    if name == 'agent_editor_close':
        timeout = arguments.get('timeoutSeconds', 30)
        params = ['-Receipt', receipt, '-TimeoutSeconds', str(timeout)]
        if arguments.get('discardChanges', False):
            params.append('-DiscardChanges')
        return _agent_editor_helper('Stop-AgentEditor.ps1', params, timeout + 5)
    timeout = arguments.get('timeoutMs', 45000)
    params_root = REPO / 'logs/agent-editor/requests'
    params_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='call-', dir=params_root) as temporary:
        params_path = Path(temporary) / 'params.json'
        params_path.write_text(json.dumps(arguments.get('arguments', {}), ensure_ascii=False), encoding='utf-8')
        return _agent_editor_helper('Invoke-AgentEditor.ps1', ['-Receipt', receipt, '-Method', arguments.get('method', ''),
                                   '-ParamsPath', str(params_path), '-TimeoutMs', str(timeout)], timeout / 1000 + 15)


AGENT_EDITOR_TOOLS = [
    {'name': 'agent_editor_launch',
     'description': 'Launch an owned, named editor on an explicit control port, in the background by default. Returns an ownership receipt and cleanup reminder. Use agent_editor_call with that receiptPath for every call to this window; default engine tools may address another editor. Close it with agent_editor_close when finished.',
     'inputSchema': {'type': 'object', 'properties': {
         'project': {'type': 'string'}, 'name': {'type': 'string'},
         'port': {'type': 'integer', 'minimum': 1, 'maximum': 65535},
         'background': {'type': 'boolean', 'default': True}, 'buildDir': {'type': 'string'},
         'timeoutSeconds': {'type': 'integer', 'minimum': 1, 'maximum': 600, 'default': 90},
     }, 'required': ['project', 'name', 'port']}},
    {'name': 'agent_editor_call',
     'description': 'Call a wire method (such as info or scene.create) or MCP tool name (such as engine_info) on the editor identified by receiptPath. Validates PID, start time, executable, port, project, name, and autosave policy before calling. Uses that editor\'s manifest and never the default control port. Keep the receipt for agent_editor_close cleanup.',
     'inputSchema': {'type': 'object', 'properties': {
         'receiptPath': {'type': 'string'}, 'method': {'type': 'string'},
         'arguments': {'type': 'object', 'default': {}},
         'timeoutMs': {'type': 'integer', 'minimum': 1, 'maximum': 120000, 'default': 45000},
     }, 'required': ['receiptPath', 'method']}},
    {'name': 'agent_editor_close',
     'description': 'Gracefully close only the editor owned by receiptPath and validate its exit. Saves changes by default; discardChanges must be explicit. Never kills another process. Returns cleanupRequired=false only after this owned editor exits.',
     'inputSchema': {'type': 'object', 'properties': {
         'receiptPath': {'type': 'string'}, 'discardChanges': {'type': 'boolean', 'default': False},
         'timeoutSeconds': {'type': 'integer', 'minimum': 1, 'maximum': 120, 'default': 30},
     }, 'required': ['receiptPath']}},
]

RUN_BROWSER_GAUNTLET = {
    'name': 'run_browser_gauntlet',
    'description': 'Build automation and production browser artifacts, run bounded headless hardware Chromium checks, verify the production bridge is absent, and return a versioned web-only report. Does not require or open an editor. Currently qualifies AgentPipelineRust; selected Rust and C# games use gameplay_job_start with target web and operation publish. Browser processes and local servers are owned and closed by the runner.',
    'inputSchema': {'type': 'object', 'properties': {
        'project': {'type': 'string', 'default': 'projects/AgentPipelineRust'},
        'reportPath': {'type': 'string', 'default': 'logs/browser-report.json'},
        'skipBuild': {'type': 'boolean', 'default': False},
        'playwrightModule': {'type': 'string'}, 'browserExecutable': {'type': 'string'},
        'timeoutSeconds': {'type': 'integer', 'minimum': 1, 'maximum': 1200, 'default': 240},
    }},
}


def _run_browser_gauntlet(arguments: dict) -> dict:
    seconds = arguments.get('timeoutSeconds', 240)
    if type(seconds) is not int or not 1 <= seconds <= 1200:
        raise ValueError('Pog Champ, timeoutSeconds must be 1..1200')
    report = Path(arguments.get('reportPath', 'logs/browser-report.json'))
    if not report.is_absolute():
        report = REPO / report
    params = [PWSH, '-NoProfile', '-File', str(REPO / 'scripts/Run-BrowserGauntlet.ps1'),
              '-Project', arguments.get('project', 'projects/AgentPipelineRust'), '-ReportPath', str(report), '-TimeoutSeconds', str(seconds)]
    if arguments.get('skipBuild', False):
        params.append('-SkipBuild')
    for key, flag in (('playwrightModule', '-PlaywrightModule'), ('browserExecutable', '-BrowserExecutable')):
        if arguments.get(key):
            params.extend([flag, arguments[key]])
    report.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen(params, cwd=REPO, stdout=output, stderr=output,
                                   creationflags=(subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP) if os.name == 'nt' else 0,
                                   start_new_session=os.name != 'nt')
        try:
            code = process.wait(timeout=seconds + 30)
            output.seek(0)
            response_text = output.read().decode('utf-8-sig')
        except subprocess.TimeoutExpired:
            if os.name == 'nt':
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True, timeout=30,
                               creationflags=subprocess.CREATE_NO_WINDOW)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=30)
            return {'result': {'schemaVersion': 1, 'status': 'cancelled', 'target': 'web', 'phase': 'timeout',
                               'diagnostics': [{'code': 'BROWSER_TIMEOUT', 'message': 'Owned browser process tree exceeded its bound'}]},
                    'error': 'Pog Champ, browser verification timed out'}
    try:
        payload = json.loads(response_text)
    except (OSError, ValueError) as exc:
        return {'error': f'Pog Champ, browser runner produced no readable report: {exc}'}
    if code != 0 and payload.get('status') == 'passed':
        payload['status'] = 'failed'
        payload.setdefault('diagnostics', []).append({'code': 'BROWSER_PROCESS_EXIT', 'message': 'Browser process exited unsuccessfully'})
    result = {'result': payload}
    if code != 0 or payload.get('status') != 'passed':
        result['error'] = 'Pog Champ, browser verification ' + str(payload.get('status', 'failed'))
    return result


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


GAMEPLAY_ASSERTION_SCHEMA = {'type': 'object', 'properties': {
    'entity': {'type': 'string'}, 'type': {'type': 'string'}, 'property': {'type': 'string'},
    'equals': {}, 'tolerance': {'type': 'number', 'minimum': 0}
}, 'required': ['entity', 'type', 'property', 'equals']}

GAMEPLAY_JOB_START = {
    'name': 'gameplay_job_start',
    'description': 'Start a persistent gameplay pipeline job and return its queued report immediately. Create, inspect, build, bounded run/relaunch/test, or publish a desktop/Web project without a running editor. Web publishing runs target preflight and relocated browser verification. Poll gameplay_job_status; cancel only this job with gameplay_job_cancel.',
    'inputSchema': {
        'type': 'object',
        'properties': {
            'project': {'type': 'string', 'description': 'Project root path; create uses the destination root.'},
            'operation': {'type': 'string', 'enum': list(gameplay_jobs.OPERATIONS)},
            'target': {'type': 'string', 'enum': ['desktop', 'web'], 'default': 'desktop'},
            'scene': {'type': 'string'},
            'name': {'type': 'string'},
            'template': {'type': 'string'},
            'language': {'type': 'string'},
            'ticks': {'type': 'integer', 'minimum': 1, 'maximum': 1000000, 'default': 60},
            'step': {'type': 'number', 'exclusiveMinimum': 0, 'maximum': 1, 'default': 1 / 60},
            'timeoutMs': {'type': 'integer', 'minimum': 100, 'maximum': 600000, 'default': 180000},
            'launchConfig': {
                'type': 'object',
                'properties': {
                    'profile': {'type': 'string', 'enum': ['native-headless-2d-v1', 'native-desktop-2d-v1'], 'default': 'native-headless-2d-v1'},
                    'readSaves': {'type': 'boolean', 'default': False},
                    'input': {'type': 'array', 'items': {'type': 'object', 'properties': {
                        'tick': {'type': 'integer', 'minimum': 0}, 'key': {'type': 'string'}, 'down': {'type': 'boolean'}
                    }, 'required': ['tick', 'key', 'down']}},
                    'assertions': {'type': 'array', 'items': GAMEPLAY_ASSERTION_SCHEMA},
                    'checkpoints': {'type': 'array', 'description': 'Desktop profile only; assert after these completed simulation ticks.',
                        'items': {'type': 'object', 'properties': {
                            'tick': {'type': 'integer', 'minimum': 1, 'maximum': 1000000},
                            'assertions': {'type': 'array', 'minItems': 1, 'items': GAMEPLAY_ASSERTION_SCHEMA}
                        }, 'required': ['tick', 'assertions']}},
                },
            },
        },
        'required': ['project', 'operation'],
    },
}

GAMEPLAY_JOB_START['inputSchema']['properties']['cases'] = {
    'type': 'array', 'minItems': 1, 'maxItems': 32,
    'description': 'Desktop test only. Build once, then run each named case in a fresh process. Omitted scene/ticks/step/launchConfig inherit the top-level values; launchConfig replaces the whole object. Saves remain project-scoped.',
    'items': {'type': 'object', 'additionalProperties': False, 'required': ['name'], 'properties': {
        'name': {'type': 'string', 'minLength': 1, 'maxLength': 128, 'description': 'Unique nonblank name, at most 128 UTF-8 bytes, no NUL.'},
        'scene': {'type': 'string'},
        'ticks': {'type': 'integer', 'minimum': 1, 'maximum': 1000000},
        'step': {'type': 'number', 'exclusiveMinimum': 0, 'maximum': 1},
        'launchConfig': GAMEPLAY_JOB_START['inputSchema']['properties']['launchConfig']
    }}
}

GAMEPLAY_JOB_TOOLS = [GAMEPLAY_JOB_START] + [
    {'name': 'gameplay_job_' + action,
     'description': description + ' Works across MCP restarts and needs no editor endpoint.',
     'inputSchema': {'type': 'object', 'properties': {'jobId': {'type': 'string'}}, 'required': ['jobId']}}
    for action, description in (
        ('status', 'Read the persisted snapshot or terminal schema-v1 report for one gameplay job.'),
        ('cancel', 'Request cancellation of this owned gameplay job and its worker tree; poll status for terminal cleanup.'),
    )
]


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
    return [RUN_GAUNTLET, RUN_BROWSER_GAUNTLET, KENNEY_LIST_PACKS, KENNEY_LIST_MODELS, KENNEY_IMPORT, KENNEY_IMPORT_PACK, KENNEY_IMPORT_FONT] + GAMEPLAY_JOB_TOOLS + AGENT_EDITOR_TOOLS + _engine_tools()


def _call_tool(name: str, arguments: dict) -> dict:
    if name in ('agent_editor_launch', 'agent_editor_call', 'agent_editor_close'):
        return _agent_editor(name, arguments)
    if name in ('gameplay_job_start', 'gameplay_job_status', 'gameplay_job_cancel'):
        if name == 'gameplay_job_start':
            result = gameplay_jobs.start_job(arguments, repo=REPO)
        elif name == 'gameplay_job_status':
            result = gameplay_jobs.status_job(arguments.get('jobId'), repo=REPO)
        else:
            result = gameplay_jobs.cancel_job(arguments.get('jobId'), repo=REPO)
        response = {'result': result}
        if result['report']['status'] in ('failed', 'cancelled', 'skipped'):
            response['error'] = 'Gameplay job ' + result['report']['status']
        return response
    if name == "run_browser_gauntlet":
        return _run_browser_gauntlet(arguments)
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
        response = {"content": [{"type": "text", "text": text}], "isError": is_error}
        if isinstance(payload, dict):
            response['structuredContent'] = payload
        return _result(msg_id, response)

    if msg_id is not None:
        return _error(msg_id, -32601, f"method not found: {method}")
    return None


def main():
    sys.stdin.reconfigure(encoding='utf-8')
    sys.stdout.reconfigure(encoding='utf-8')
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
