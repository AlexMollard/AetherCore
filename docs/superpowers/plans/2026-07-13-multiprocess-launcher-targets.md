# Multi-Process Launcher & Target Rework — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status (2026-07-13):** Phases A–D implemented, pushed, and **build-verified on Windows** (MSVC/Debug):
- CMake configures clean (new `Launcher` target, folders, `CompileShaders` rename, `get_target_property` config-copy).
- All four targets compile + link: `Editor.exe`, `Launcher.exe`, `AetherGame.exe` (GameRuntime), `EngineTests.exe`.
- `ParseProjectArg` doctest passes.
- `Editor.exe` with no `--project` errors and exits 2 (Phase D); `Editor.exe --project <TestingProject>` boots the editor (survives startup, no crash dump); `Launcher.exe` boots the hub.
- Only un-automated check: the GUI "Open" click → Editor-process spawn. Its parts are all verified (Editor `--project` boots; `Editor.exe` sits next to `Launcher.exe`; `SpawnEditor` compiles + the target resolves).

Deviation from the plan: C+D were implemented via a shared-sources `AETHERCORE_LAUNCHER` define (the Launcher reuses the Editor sources; `OpenProject` spawns) rather than extracting a `ProjectCommon` lib — this built clean.

**Goal:** Make the launcher a separate process (spawns the editor), rename `App → Editor` (+ new `Launcher`, `App_CompileShaders → CompileShaders`), organize targets into Solution Explorer folders, and make the editor always project-scoped (`Editor --project <path>`).

**Architecture:** Two executables from mostly-shared code: `Launcher` (project hub → spawns `Editor --project`) and `Editor` (the renamed `App`, boots straight into a project). `GameRuntime` unchanged. A separate process gives a real separate OS window with no ImGui-viewport gymnastics (the superseded approach). Phased so the build works after every phase.

**Tech Stack:** C++23, CMake (MSVC `build/vs2022-msvc`), Dear ImGui, GLFW, Vulkan, doctest, PowerShell dev scripts.

**Spec:** `docs/superpowers/specs/2026-07-13-multiprocess-launcher-targets-design.md`

---

## Verification model (read first)

Windowing/build/process feature — **not headlessly verifiable here**. Each phase ends
with build + a manual "drive it" check on your machine. Reference commands (note the
target renames as they land):

- **Configure (needed after CMake target changes):** `cmake --preset vs2022-msvc`
- **Build a target:** `cmake --build build/vs2022-msvc --config Debug --target Editor`
- **Run from the build-tree root** (so `shaders://` resolves): `cd build/vs2022-msvc && ./src/app/Debug/Editor.exe --project D:/AetherCore/projects/TestingProject`
- **Unit tests:** `cmake --build build/vs2022-msvc --config Debug --target EngineTests && ./build/vs2022-msvc/tests/Debug/EngineTests.exe --test-case="<name>"`

Do phases **in order**; each is independently buildable/runnable.

---

## File structure

- `src/app/CMakeLists.txt` — rename `App` target → `Editor`; new `Launcher` target; folders.
- `src/app/main.cpp` — CLI parsing (`--project`); editor boots from the arg.
- `src/app/ProjectCli.hpp` / `.cpp` **(new)** — tiny pure `--project` arg parser (unit-tested).
- `src/app/launcher/LauncherMain.cpp` **(new)** — the `Launcher` executable: boot + draw the hub + spawn `Editor`.
- `src/app/launcher/LauncherProcess.hpp` / `.cpp` **(new)** — detached child-process spawn (`Editor --project <path>`), Windows-first.
- `src/app/debug/EditorProjectManager.*`, `src/app/debug/ProjectLauncherWindow.*` — the launcher UI + project listing move to the `Launcher` side; project **loading** stays in the editor (driven by `--project`).
- `CMake/SlangShaders.cmake` + root `CMakeLists.txt` — `App_CompileShaders` → `CompileShaders`; target grouping.
- `scripts/*`, `game/AetherGame/Properties/launchSettings.json`, `tools/mcp/aethercore_mcp.py` + `tools/mcp/README.md`, `AGENTS.md`, `README.md` — `App` → `Editor` references.
- `tests/app/ProjectCliTests.cpp` **(new)** — doctest for the arg parser.

---

## Phase A — Rename `App → Editor`, `App_CompileShaders → CompileShaders`, + folders

No behavior change: `Editor` still shows the launcher at startup (as `App` did). Pure
rename + Solution Explorer organization.

### Task A1: Rename the CMake targets

**Files:** `src/app/CMakeLists.txt`, `CMake/SlangShaders.cmake`, root `CMakeLists.txt`
(any `add_subdirectory`/target refs).

- [ ] **Step 1:** In `src/app/CMakeLists.txt`, rename the editor executable:
  `add_executable(App …)` → `add_executable(Editor …)`, and update **every** `App`
  reference in that file — `target_sources(App …)`, `target_include_directories(App …)`,
  `target_link_libraries(App …)`, `target_compile_definitions(App …)`,
  `aethercore_target_defaults(App)`, `aethercore_enable_dead_strip_report(App)`,
  `aethercore_enable_slang_shader_compilation(App)`, the
  `foreach(_aether_runtime_target IN ITEMS App GameRuntime)` loops, and the
  `set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT App)` in the root `CMakeLists.txt`
  → `Editor`.

- [ ] **Step 2:** `App_CompileShaders → CompileShaders`. The name derives from the
  `${target_name}_CompileShaders` in `CMake/SlangShaders.cmake:109`
  (`add_custom_target(${target_name}_CompileShaders …)`). Since the target is now
  `Editor`, the shader target becomes `Editor_CompileShaders`. To get the plain
  `CompileShaders` name the spec wants, pass an explicit name: change the function to
  create `CompileShaders` (a fixed name) rather than `${target_name}_CompileShaders`,
  and update the `add_dependencies(${target_name} …)` accordingly. Keep the
  `App_CompileShaders`-era comment references in `src/app/CMakeLists.txt` in sync.

- [ ] **Step 3: Configure + build.** `cmake --preset vs2022-msvc` then
  `cmake --build build/vs2022-msvc --config Debug --target Editor`
  Expected: configures and builds; produces `Editor.exe`.

- [ ] **Step 4: Grep for stragglers.**
  Run: `git grep -nE '\bApp\b|App_CompileShaders' -- 'CMake/**' '**/CMakeLists.txt'`
  Expected: no first-party `App`/`App_CompileShaders` target references remain (matches
  in comments/third-party are fine).

- [ ] **Step 5: Commit.**

```bash
git add CMake/SlangShaders.cmake src/app/CMakeLists.txt CMakeLists.txt
git commit -m "Rename App target -> Editor; App_CompileShaders -> CompileShaders"
```

### Task A2: Update scripts / launchSettings / MCP / docs to `Editor`

**Files:** `scripts/Run-DebugGauntlet.ps1`, `scripts/Export-Assets.ps1` (and any other
script that builds `App` / runs `App.exe`), `game/AetherGame/Properties/launchSettings.json`,
`tools/mcp/aethercore_mcp.py`, `tools/mcp/README.md`, `AGENTS.md`, `README.md`.

- [ ] **Step 1:** Find every reference: `git grep -nE 'App\.exe|--target App|\bApp\b' -- 'scripts/**' 'tools/mcp/**' 'game/**' 'AGENTS.md' 'README.md' ':!docs/superpowers/**'`

- [ ] **Step 2:** Replace `--target App` → `--target Editor`, `App.exe` → `Editor.exe`,
  and the `src/app/Debug/App.exe` output path → `src/app/Debug/Editor.exe` (the CMake
  target name sets the exe name; confirm the output subdir stays `src/app`). Update the
  `launchSettings.json` `executablePath`/`workingDirectory` to the `Editor` output.

- [ ] **Step 3: Verify the gauntlet path.** `./scripts/Run-DebugGauntlet.ps1 -CI`
  Expected: builds `Editor` + `GameRuntime` + `EngineTests` and runs unit tests (green).

- [ ] **Step 4: Commit.**

```bash
git add scripts/ tools/mcp/ game/AetherGame/Properties/launchSettings.json AGENTS.md README.md
git commit -m "Point scripts, launchSettings, MCP tool and docs at the Editor target"
```

### Task A3: Organize targets into Solution Explorer folders

**Files:** `src/app/CMakeLists.txt`, `tools/**/CMakeLists.txt`, `tests/CMakeLists.txt`,
`CMake/*.cmake` (for the helper targets), root `CMakeLists.txt`.

- [ ] **Step 1:** Ensure folders are enabled once (root `CMakeLists.txt`):
  `set_property(GLOBAL PROPERTY USE_FOLDERS ON)` and route the CMake predefined targets:
  `set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")`.

- [ ] **Step 2:** Set `FOLDER` on each first-party target:

```cmake
set_target_properties(Editor Launcher GameRuntime PROPERTIES FOLDER "Apps")
set_target_properties(AssetPacker AssetPipeline aether-ctl PROPERTIES FOLDER "Tools")
set_target_properties(EngineTests PROPERTIES FOLDER "Tests")
# Build helpers (guard each with if(TARGET x) since some are optional):
foreach(_t ManagedAssemblies CompileShaders PackageGame clean-all dead-strip-report uninstall)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES FOLDER "Build")
    endif()
endforeach()
```

  Place these after all targets are defined (end of root `CMakeLists.txt`). `Launcher`
  is added in Phase C — until then, drop it from the `Apps` line (or guard with
  `if(TARGET Launcher)`).

- [ ] **Step 3:** For third-party deps that land ungrouped, set their `FOLDER` to
  `Dependencies` where they don't already (e.g. after `include(Dependencies)`:
  `foreach(_dep enet freetype glfw glm imgui Jolt libzstd_static TracyClient vk-bootstrap update_mappings) if(TARGET ${_dep}) set_target_properties(${_dep} PROPERTIES FOLDER "Dependencies") endif() endforeach()`).

- [ ] **Step 4: Configure + open.** `cmake --preset vs2022-msvc`, open the `.sln`, and
  confirm the Solution Explorer shows `Apps/`, `Tools/`, `Tests/`, `Build/`,
  `Dependencies/`, `CMake/`, with `Engine` at top level and no stray ungrouped
  first-party targets.

- [ ] **Step 5: Commit.**

```bash
git add CMakeLists.txt src/app/CMakeLists.txt
git commit -m "Group solution targets into Apps/Tools/Tests/Build/Dependencies folders"
```

---

## Phase B — `Editor` accepts `--project` and boots straight in

### Task B1: Add a `--project` argument parser (unit-tested)

**Files:** Create `src/app/ProjectCli.hpp`, `src/app/ProjectCli.cpp`,
`tests/app/ProjectCliTests.cpp`; modify `tests/CMakeLists.txt` (add the test source and,
if there's no app-layer test yet, add `src/app/ProjectCli.cpp` to the `EngineTests`
sources or make the parser header-only inline to avoid a link dependency).

- [ ] **Step 1: Write the failing test.**

```cpp
// tests/app/ProjectCliTests.cpp
#include <doctest/doctest.h>
#include "ProjectCli.hpp"

using aether::app::ParseProjectArg;

TEST_CASE("ParseProjectArg returns the path after --project")
{
    const char* argv[] = {"Editor", "--project", "D:/proj/Test"};
    CHECK(ParseProjectArg(3, const_cast<char**>(argv)) == "D:/proj/Test");
}
TEST_CASE("ParseProjectArg returns empty when absent or malformed")
{
    const char* a[] = {"Editor"};
    CHECK(ParseProjectArg(1, const_cast<char**>(a)).empty());
    const char* b[] = {"Editor", "--project"}; // no value
    CHECK(ParseProjectArg(2, const_cast<char**>(b)).empty());
}
```

- [ ] **Step 2: Run to verify it fails.**
  `cmake --build build/vs2022-msvc --config Debug --target EngineTests`
  Expected: FAIL to compile (`ProjectCli.hpp` not found).

- [ ] **Step 3: Implement (header-only to avoid link wiring).**

```cpp
// src/app/ProjectCli.hpp
#pragma once
#include <string>
#include <string_view>
namespace aether::app
{
    // The value after "--project", or "" if not present / missing its value.
    [[nodiscard]] inline std::string ParseProjectArg(int argc, char** argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (std::string_view(argv[i]) == "--project")
            {
                return std::string(argv[i + 1]);
            }
        }
        return {};
    }
}
```

- [ ] **Step 4: Run to verify it passes.**
  `cmake --build build/vs2022-msvc --config Debug --target EngineTests && ./build/vs2022-msvc/tests/Debug/EngineTests.exe --test-case="ParseProjectArg*"`
  Expected: PASS (2 cases).

- [ ] **Step 5: Commit.**

```bash
git add src/app/ProjectCli.hpp tests/app/ProjectCliTests.cpp tests/CMakeLists.txt
git commit -m "Add --project CLI arg parser for the Editor"
```

### Task B2: Editor boots the given project (skips the launcher when `--project` present)

**Files:** `src/app/main.cpp` (add `int argc, char** argv`; parse `--project`), and the
boot path that decides launcher-vs-project (`src/app/debug/EditorProjectManager.cpp`
`LoadSettings`/reopen path + `src/app/Application.cpp`).

- [ ] **Step 1:** `int main(int argc, char** argv)`; `const std::string project = aether::app::ParseProjectArg(argc, argv);`

- [ ] **Step 2:** When `project` is non-empty, drive `EditorProjectManager::OpenProject(project)`
  on boot and keep the launcher closed. When empty, keep the current launcher-at-startup
  behavior (transitional — Phase D removes it). Thread the parsed path from `main` to the
  editor layer (e.g. via the engine config or a service the `DebugLayer` reads on attach).

- [ ] **Step 3: Build + manual verify.**
  `cd build/vs2022-msvc && ./src/app/Debug/Editor.exe --project D:/AetherCore/projects/TestingProject`
  Expected: boots straight into the editor with TestingProject loaded, no launcher. Run
  with no args → launcher shows as before.

- [ ] **Step 4: Commit.**

```bash
git add src/app/main.cpp src/app/Application.cpp src/app/debug/EditorProjectManager.cpp
git commit -m "Editor boots straight into the project given by --project"
```

### Task B3: Editor F5 debug argument

**Files:** `src/app/CMakeLists.txt`

- [ ] **Step 1:** After the `Editor` target + `VS_DEBUGGER_WORKING_DIRECTORY`, add:

```cmake
set_target_properties(Editor PROPERTIES
    VS_DEBUGGER_COMMAND_ARGUMENTS "--project \"${AETHERCORE_PROJECT_DIR}\"")
```

- [ ] **Step 2: Configure + F5 verify.** `cmake --preset vs2022-msvc`, set `Editor` as
  the VS startup project, F5. Expected: boots into TestingProject, breakpoints hit.

- [ ] **Step 3: Commit.**

```bash
git add src/app/CMakeLists.txt
git commit -m "Editor F5 defaults to --project TestingProject"
```

---

## Phase C — `Launcher` target + spawn

### Task C1: Extract the shared project-descriptor code

**Files:** the project descriptor/context types + reader (`EditorProjectContext`,
`ReadProjectDescriptor`, recent-projects listing) currently in
`src/app/debug/EditorProjectManager.*` / `src/app/editor/EditorProjectContext.hpp`.

> Exact file boundaries finalize during implementation once the compiler reveals the
> coupling; the intent is fixed:

- [ ] **Step 1:** Move the project **descriptor** pieces both executables need
  (`EditorProjectContext`, descriptor reading, recent-projects list load/save) into a
  small static lib target `ProjectCommon` (`src/app/project/`), or into `Engine` if they
  have no editor dependencies. `Launcher` and `Editor` both link it.

- [ ] **Step 2:** Leave project **loading** (`OpenProject` → mount, compile shaders,
  build scripts, load scene) in the editor — the launcher never loads in-process.

- [ ] **Step 3: Build both existing targets** (`Editor`, `EngineTests`) to confirm the
  extraction didn't break the editor.
  Run: `cmake --preset vs2022-msvc && cmake --build build/vs2022-msvc --config Debug --target Editor EngineTests`
  Expected: builds clean.

- [ ] **Step 4: Commit.**

```bash
git add -A && git commit -m "Extract shared project-descriptor code into ProjectCommon"
```

### Task C2: Process-spawn helper

**Files:** Create `src/app/launcher/LauncherProcess.hpp`, `.cpp`.

- [ ] **Step 1:** Implement a detached spawn of `Editor --project <path>`. Reuse the
  existing process plumbing style (`src/engine/io/Process.cpp`). Resolve the `Editor`
  executable next to the current (launcher) executable. Windows-first (`CreateProcessA`
  with `DETACHED_PROCESS`), matching the current dev target.

```cpp
// src/app/launcher/LauncherProcess.hpp
#pragma once
#include <filesystem>
namespace aether::app::launcher
{
    // Spawn "Editor --project <projectRoot>" as a detached process. Returns true if the
    // process started. `editorExe` is the Editor executable path (resolved next to the
    // launcher exe by the caller).
    bool SpawnEditor(const std::filesystem::path& editorExe, const std::filesystem::path& projectRoot);
}
```

- [ ] **Step 2:** Implement `SpawnEditor` in the `.cpp` (build the command line
  `"<editorExe>" --project "<projectRoot>"`, `CreateProcessA` with `DETACHED_PROCESS |
  CREATE_NEW_PROCESS_GROUP`, close handles, return the BOOL). Guard non-Windows with a
  `#else` that logs "unsupported" and returns false (dev target is Windows).

- [ ] **Step 3: Commit** (compiles once the `Launcher` target links it in C3).

```bash
git add src/app/launcher/LauncherProcess.* && git commit -m "Add detached Editor process spawn for the launcher"
```

### Task C3: The `Launcher` executable

**Files:** Create `src/app/launcher/LauncherMain.cpp`; modify `src/app/CMakeLists.txt`
(new `Launcher` target); move `ProjectLauncherWindow.*` to the launcher side.

- [ ] **Step 1:** `add_executable(Launcher …)` linking `Engine`, `ProjectCommon`, and
  the launcher sources (`LauncherMain.cpp`, `LauncherProcess.cpp`, `ProjectLauncherWindow.cpp`).
  Add it to the `Apps` folder line and the `ManagedAssemblies`/nethost copy loops if it
  needs the managed runtime (it does not build scripts, so likely only needs the window/UI —
  confirm during build). `aethercore_target_defaults(Launcher)`.

- [ ] **Step 2:** `LauncherMain.cpp`: boot the engine + ImGui (mirroring `main.cpp`'s
  boot but pushing only a launcher layer that draws `ProjectLauncherWindow`), wire the
  hub actions (Open/Continue/Create) to `launcher::SpawnEditor(editorExe, projectRoot)`
  with the `Editor` exe resolved next to the launcher exe. Keep the launcher open after
  spawning (Hub-style).

- [ ] **Step 3: Configure + build + run.**
  `cmake --preset vs2022-msvc && cmake --build build/vs2022-msvc --config Debug --target Launcher Editor`
  then `cd build/vs2022-msvc && ./src/app/Debug/Launcher.exe`
  Expected: the `Launcher` opens as its **own OS window** (own taskbar entry). Clicking
  Open/Continue on TestingProject **spawns a separate `Editor` process/window** for it;
  the launcher stays open.

- [ ] **Step 4: Commit.**

```bash
git add -A && git commit -m "Add Launcher executable that spawns the Editor process"
```

---

## Phase D — Editor drops the in-app launcher (always project-scoped)

### Task D1: Require `--project`; remove the editor's launcher + no-project state

**Files:** `src/app/main.cpp`, `src/app/debug/EditorProjectManager.*`,
`src/app/layers/DebugLayer.cpp` (the launcher-draw branch + no-project backdrop path),
`src/app/CMakeLists.txt` (drop `ProjectLauncherWindow.cpp` from `Editor` if still listed).

- [ ] **Step 1:** In `main.cpp`, if `--project` is empty, log a clear error
  ("Editor requires --project <path>; launch it from the Launcher") and exit non-zero
  (the editor is always project-scoped now).

- [ ] **Step 2:** Remove the launcher-at-startup path from the editor: the
  `DrawLauncher()` branch + `m_launcherOpen`/`OpenLauncher`/`CloseLauncher` usage in
  `DebugLayer`/`EditorProjectManager`, and the no-project backdrop early-return added in
  the superseded Phase 3a. The editor always has a loaded project, so the
  `if (!IsProjectLoaded())` branches simplify away.

- [ ] **Step 3:** Remove `ProjectLauncherWindow.*` from the `Editor` target's sources
  (it lives on the launcher now).

- [ ] **Step 4: Build + manual verify.**
  Run `Launcher` → open TestingProject → editor spawns and works. Run
  `Editor.exe` with no args → clean error + exit. F5 `Editor` (has the `--project`
  default) → boots into the project.
  Expected: no launcher UI inside the editor; no no-project state.

- [ ] **Step 5: Full gauntlet.** `./scripts/Run-DebugGauntlet.ps1 -CI`
  Expected: green (builds `Editor`/`GameRuntime`/`EngineTests`, unit tests pass).

- [ ] **Step 6: Commit.**

```bash
git add -A && git commit -m "Editor is always project-scoped; remove its in-app launcher"
```

---

## Self-review notes

- **Spec coverage:** targets/renames (A1), scripts/docs churn (A2), solution folders
  (A3), `--project` + F5 debug (B1–B3), shared code + spawn + Launcher exe (C1–C3),
  editor-always-project-scoped + no-project removal (D1). `GameRuntime` untouched. DPI /
  the superseded viewport work are not re-touched.
- **Known soft spots (by design, not placeholders):** C1's exact file boundaries and
  C3's engine-boot reuse depend on the current coupling a compiler reveals; the plan
  fixes the *intent* (shared `ProjectCommon`, launcher-only `ProjectLauncherWindow`,
  full-engine launcher boot) and the exact source lists finalize during implementation.
  The `CompileShaders` rename (A1 Step 2) may need the shader target's dependents checked.
- **Type/name consistency:** `Editor` target, `Launcher` target, `CompileShaders`
  target, `ParseProjectArg`, `SpawnEditor`, `ProjectCommon` lib — used consistently
  across tasks.
