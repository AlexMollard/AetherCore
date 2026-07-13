# Multi-Process Launcher & Target Rework

**Date:** 2026-07-13
**Status:** Design approved; pending implementation plan
**Supersedes:** `2026-07-13-separate-launcher-window-design.md` (the ImGui-multi-viewport
approach to a separate launcher window — abandoned; see below).

## Problem

Two related issues:

1. **The launcher can't become a real separate window in-process.** The ImGui
   multi-viewport approach (per-window `ViewportFlagsOverrideSet = NoAutoMerge`, then
   global `io.ConfigViewportsNoAutoMerge`) both failed to promote the launcher to its
   own OS window in this custom, threaded viewport renderer — it only creates platform
   windows for *drag-created* viewports, not programmatically-promoted ones. The
   launcher renders as a centered card inside the main window instead.
2. **The `App` target name is wrong** — `App` *is* the editor. The first-party targets
   are also ungrouped in the Solution Explorer, so the root is cluttered.

A separate **process** gives a guaranteed separate OS window with no viewport gymnastics
— the Unity-Hub / Epic-Launcher model. This also composes with the project-scoped editor
we've been building (runtime-owned scripts, project-relative builds): the editor is
always launched *with* a project, which removes the awkward no-project editor state.

## Goals

- The project launcher is a **separate process** (`Launcher`) with its own real OS
  window; opening/creating a project **spawns the editor process** for that project.
- Clear target names: `App` → **`Editor`**, new **`Launcher`**, `App_CompileShaders` →
  **`CompileShaders`**; `GameRuntime` unchanged.
- First-party targets organized into **Solution Explorer folders**.
- The **editor is always project-scoped** (`Editor --project <path>`); its startup
  launcher branch and no-project handling are removed.
- **VS debugging is preserved**: `Editor` and `Launcher` are each independently
  F5-debuggable; `Editor` has a default `--project` debug argument so F5 drops straight
  into the editor.

## Non-goals

- A lighter window-only launcher boot. The `Launcher` boots the **full engine**
  initially (simplest, reuses the existing boot; no startup regression vs. today, where
  `App` already boots the full engine to show the launcher). A lean launcher-only boot
  is future work.
- Any change to `GameRuntime`, the asset tooling, or `Engine` internals beyond target
  renames/folders.
- Inter-process communication beyond a command-line argument. The launcher passes the
  project path when spawning the editor; nothing streams back.

## Design

### Targets & names

| Now | After | Notes |
|---|---|---|
| `App` (editor exe) | `Editor` | renamed; drops the startup launcher; reads `--project` |
| *(new)* | `Launcher` | new exe; links `Engine`; drives the launcher UI; spawns `Editor` |
| `App_CompileShaders` | `CompileShaders` | drop the `App_` prefix |
| `GameRuntime` | `GameRuntime` | unchanged |
| `Engine`, `AssetPacker`, `AssetPipeline`, `aether-ctl`, `EngineTests`, `ManagedAssemblies`, `PackageGame` | (same) | unchanged names |

### Process model

- **`Launcher`**: boots the engine + ImGui, shows the project hub
  (`ProjectLauncherWindow`). On Open / Continue / Create it launches
  `Editor --project <path>` as a detached child process (reusing the existing
  process-spawn plumbing — the engine already shells out for `dotnet build`) and stays
  open (Hub-style; user-closeable). It never loads a project in-process.
- **`Editor`**: parses `--project <path>` in `main`, boots straight into the editor for
  that project. No launcher UI at startup; no no-project state.
- **`GameRuntime`**: unchanged.

### Code organization

- The launcher UI (`ProjectLauncherWindow` and the project **listing/browse/create**
  logic) moves to the `Launcher` target. The launcher no longer opens projects
  in-process — its "open/continue/create" actions **spawn the editor** instead.
- The `Editor` keeps project **loading** (the `EditorProjectManager::OpenProject`
  path), now driven by the `--project` argument rather than the launcher UI.
- Genuinely shared bits (project descriptor discovery, the window + ImGui boot) live in
  `Engine` (or a small shared static lib both executables link) so `Launcher` and
  `Editor` don't duplicate them.
- `main.cpp` grows CLI parsing: `--project <path>` (editor) and no-arg/`--launcher`
  is not needed since the launcher is its own target with its own entry point.

### Solution Explorer folders (`FOLDER` target property)

- **Apps/**: `Editor`, `Launcher`, `GameRuntime`
- **Tools/**: `AssetPacker`, `AssetPipeline`, `aether-ctl`
- **Tests/**: `EngineTests`, `doctest_with_main`
- **Build/**: `ManagedAssemblies`, `CompileShaders`, `PackageGame`, `clean-all`,
  `dead-strip-report`, `uninstall`
- **Dependencies/**: third-party libs (set `FOLDER` on any that don't already)
- **CMake/**: `ALL_BUILD`, `INSTALL`, `PACKAGE`, `RUN_TESTS`, `ZERO_CHECK` (via
  `CMAKE_FOLDER` / `PREDEFINED_TARGETS_FOLDER`)
- `Engine` stays a top-level project (the anchor).

### Debug

- `Editor`: `set_target_properties(Editor PROPERTIES VS_DEBUGGER_COMMAND_ARGUMENTS
  "--project <AETHERCORE_PROJECT_DIR>")` and keep the existing
  `VS_DEBUGGER_WORKING_DIRECTORY`, so F5 on `Editor` boots straight into TestingProject.
- `Launcher`: F5-debuggable directly. Spawned child editors aren't auto-attached (rare
  need; VS child-process auto-attach handles it if ever wanted).

## Churn — `App` → `Editor` (and `App_CompileShaders` → `CompileShaders`)

`App` is referenced widely; the plan enumerates and updates every site:
- **CMake**: `add_executable(App)` / target properties / `add_dependencies` /
  `target_compile_definitions(App …)` / the `App_CompileShaders` target and its
  `aethercore_enable_slang_shader_compilation(App)` call / the `ManagedAssemblies`
  runtime-target loop (`App GameRuntime`) / asset-payload POST_BUILD.
- **Scripts**: `Run-DebugGauntlet.ps1`, `Export-Assets.ps1`, and any `--target App` /
  `App.exe` path.
- **`game/AetherGame/Properties/launchSettings.json`** — already updated to
  `build/vs2022-msvc/src/app/Debug/App.exe`; repoint to the `Editor` output.
- **MCP tool** (`tools/mcp/aethercore_mcp.py`) — any `App.exe` / `--target App`.
- **Docs** (`AGENTS.md`, `README.md`, `tools/mcp/README.md`, archived plans left as-is).

## Testing

- **Build**: all targets compile; `Launcher`, `Editor`, `GameRuntime` link.
- **Manual (driven by the user; UI/process, not headless):**
  1. Run `Launcher` → its own OS window (own taskbar entry) showing the project hub.
  2. Open/Continue a project → a **separate `Editor` process/window** opens for it; the
     launcher stays open.
  3. Create a project → same spawn path.
  4. F5 the `Editor` target in VS → boots straight into TestingProject, breakpoints hit.
  5. `GameRuntime` still builds/runs.
  6. The gauntlet (`Run-DebugGauntlet.ps1`) builds the renamed targets and passes.
- **Unit**: CLI-arg parsing (`--project <path>` → resolved project path) is a small pure
  function → doctest.

## Risks

- **Wide rename.** `App → Editor` touches CMake, scripts, launchSettings, the MCP tool,
  and docs; a missed reference breaks a build/run path. Mitigated by enumerating sites
  in the plan and a final `grep` sweep.
- **Code extraction.** Splitting the launcher UI out of the editor target must not leave
  the editor depending on launcher-only code (or vice versa); keep shared code in
  `Engine`/a shared lib.
- **Process spawn portability.** Detached child-process launch is implemented per-OS
  (Windows first, matching the current dev target); reuse the existing process plumbing.
- Cannot be built/verified in this environment — each piece is built and driven by the
  user.

## Future work

- Lean launcher boot (window + ImGui only, no full engine) for faster launcher startup.
- Launcher ↔ editor IPC (e.g. reflect "editor closed" back to the hub) if desired.
