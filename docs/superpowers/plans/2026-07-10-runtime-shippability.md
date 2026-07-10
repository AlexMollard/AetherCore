# Runtime Shippability Hardening Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Fresh implementer per task + spec then quality review. Build/verify under MSVC (`build-vs2022-msvc`). Branch: `feature/shader-asset-pipeline` (continues on the same branch; will be pushed combined for review).

**Goal:** Make the shipped `GameRuntime` (`AetherGame.exe`) a clean, standalone game runtime. It was never run before (publish failed at error 123 until the asset-pipeline work), so it still carries editor/dev subsystems and doesn't get project content. Fix four things: (R1) load the project's actual startup scene, (R2) don't pollute the game's working dir with runtime-written files, (R3) don't ship/enable dev-only GPU diagnostics (NVIDIA Aftermath) — and make it AMD-safe, (R4) don't link or init Dear ImGui in the runtime.

**Context (diagnosis):** `GameRuntime` and `App` share `src/app/main.cpp` + `Application`; only `DebugLayer` (and `src/app/debug/`, `src/app/editor/`) is `AETHERCORE_EDITOR_APP`-gated / source-filtered out of the runtime. `ImguiSubsystem` lives in the **Engine** lib (`src/engine/imgui/`), so the runtime links imgui and `Application` creates it unconditionally. Aftermath is compile-time `AETHER_ENABLE_NVIDIA_AFTERMATH` (baked in when the build box has the SDK) and shipped into both exes. The runtime loads `app.startupScene` from the shipped engine `EngineSettings.toml`, but the project's startup scene lives in `ProjectSettings.toml` and isn't propagated to the published build.

**Tech Stack:** C++20, CMake, Vulkan, imgui, Vulkan pipeline cache, TOML settings, `aether::io::PlatformPaths` (LocalAppData).

---

## Reference files
- `src/app/main.cpp` — editor vs runtime entry (`AETHERCORE_EDITOR_APP` gates `DebugLayer`); both push `ScriptedSceneLayer`.
- `src/app/Application.cpp` — creates subsystems incl. `ImguiSubsystem` (`#include "imgui/ImguiSubsystem.hpp"`). Where imgui is created/registered/rendered.
- `src/app/layers/ScriptedSceneLayer.cpp` — the runtime's world/scene layer; how it resolves + loads the startup scene (`app.startupScene`), the `#ifdef AETHERCORE_EDITOR_APP` blocks.
- `src/app/debug/EditorProjectManager.cpp` — `LoadProjectStartupScene()` (editor path) + `ProjectSettings.toml` `app.startupScene`; how the editor knows the scene. The publish flow must carry this to the runtime.
- `src/app/editor/EditorProjectPublisher.cpp` — `PublishProject`/`CopyShippedDataPayload`/settings copy; where to write the runtime's effective startup scene + scene assets.
- `src/engine/imgui/ImguiSubsystem.{hpp,cpp}`, `src/engine/CMakeLists.txt` (how imgui is linked into Engine), render-graph imgui pass registration.
- `src/engine/vulkan/AftermathContext.cpp`, `VulkanContext.cpp` (device create + Aftermath enable), `CMake/Dependencies.cmake` / `FindNvidiaAftermath.cmake` (`NVIDIA_AFTERMATH_FOUND`, DLL copy), `src/app/CMakeLists.txt` (Aftermath DLL POST_BUILD copy + `_am_dll`).
- `src/engine/io/PlatformPaths.hpp` — LocalAppData resolution (already used for `EditorState.toml`/`UserSettings.toml`); Vulkan pipeline-cache write location (grep `PipelineCache`/`pipelinecache`/`GetPipelineCacheData` in `src/engine/vulkan`/`gpu`).

---

## R1 — Load the project's startup scene in the published game
**Goal:** A published game boots into the project's actual startup scene, not an empty world.

**Investigate first:** how `ScriptedSceneLayer` picks the startup scene at runtime (which setting: engine `app.startupScene`?), and where scene files + their referenced assets live (in `project.pak`? loose `resources/scenes`?). Confirm the scene the editor loads (`EditorProjectManager::LoadProjectStartupScene` via `ProjectSettings.toml`) vs what the runtime reads.

**Approach:** Publish must make the runtime resolve the SAME startup scene the editor uses. Likely: bake the project's `app.startupScene` (from `ProjectSettings.toml`) into the published build's effective settings (the shipped `EngineSettings.toml` or a project settings the runtime reads), AND ensure the scene file + everything it references is in `project.pak`. Keep it deterministic and documented. Add a `VerifyPublishedGame` check that the resolved startup scene asset is present in the shipped paks.

**Verify:** publish `projects/TestingProject`; confirm the shipped settings name the project's startup scene and the scene asset is in `project.pak`; (best-effort) the published game log shows it loading N scene entities, not 0.

---

## R2 — Redirect runtime-written files out of the game directory
**Goal:** The shipped game writes no files into its own program directory; runtime-writable state (Vulkan pipeline cache, any caches) goes to `%LOCALAPPDATA%/AetherCore/` (or per the existing convention). `imgui.ini` disappears once R4 lands.

**Investigate:** find every runtime disk write that lands CWD/exe-relative — Vulkan pipeline cache (`vkGetPipelineCacheData` → file), any `.spv`/shader cache write (identify the source of the reported `.spv` files!), `imgui.ini` (imgui `io.IniFilename`). Grep `src/engine` for `ofstream`/`WriteFile`/`GetPipelineCacheData`/`IniFilename`.

**Approach:** route each writable path through `PlatformPaths` (LocalAppData). For the pipeline cache, write/read it under LocalAppData keyed by app+GPU. If the `.spv` pollution is a shader/pipeline cache dump, redirect or disable it in shipped builds. Do NOT change dev/editor behavior in a way that breaks the build tree.

**Verify:** run the game from a clean dir; confirm no new files appear in the program dir (they appear under LocalAppData instead).

---

## R3 — Strip NVIDIA Aftermath (and dev-only GPU diagnostics) from the shipped runtime; AMD-safe
**Goal:** The shipped game does not carry/enable Aftermath; on a non-NVIDIA GPU it runs fine.

**Investigate:** how Aftermath is enabled at device creation (`AftermathContext`/`VulkanContext`) — does it request NVIDIA-specific device extensions/features unconditionally? If so, on AMD `vkCreateDevice` could fail. Confirm current behavior + the vendor check (if any).

**Approach:** (a) Runtime vendor-gate: only enable Aftermath / request its device features when the physical device is NVIDIA (vendorID 0x10DE) AND it's an editor/dev build — so an AMD device never requests NVIDIA extensions. (b) Make Aftermath editor/dev-only: don't compile/enable it into `GameRuntime` (a shipped game is not a GPU-crash-debugging target), and don't POST_BUILD-copy `GFSDK_Aftermath_Lib.x64.dll` into the runtime/published output. Keep it in the editor (`App`) where devs want crash dumps. Decide the cleanest gate (a compile define split between App and GameRuntime, mirroring the existing editor gating).

**Verify:** GameRuntime build/publish contains NO `GFSDK_Aftermath*.dll` and no Aftermath init in its log; editor still has it. Reason through (or, if an AMD box is available, confirm) that device creation no longer requests NVIDIA extensions on non-NVIDIA.

---

## R4 — De-link Dear ImGui from the shipped runtime
**Goal:** `GameRuntime` neither links nor initializes imgui (no `imgui.ini`, no wasted init, smaller binary). The editor keeps its imgui UI.

**Investigate (carefully):** everything that references `ImguiSubsystem`/imgui — it's created in `Application` (shared), lives in the Engine lib (`src/engine/imgui/`), and is rendered as a render-graph pass. Map: who constructs it, who renders it, and whether any ENGINE (non-editor) code depends on it (the panels in `src/app/debug/` are editor-only). 

**Approach (pick the cleanest that fully de-links from GameRuntime):**
- Move the imgui integration (`src/engine/imgui/`) out of the Engine lib into an editor-only static library (e.g. `EditorUI`) that ONLY `App` links; Engine stops linking imgui. `Application`'s imgui creation + the imgui render pass become `AETHERCORE_EDITOR_APP`-gated (or live in the editor-only path). GameRuntime links neither.
- If a full lib-split is too invasive, at minimum: (a) `Application` only creates/renders imgui under `AETHERCORE_EDITOR_APP`, AND (b) exclude the imgui sources + imgui third-party from the GameRuntime link (so it genuinely doesn't link imgui). Prefer the real de-link over just gating init.
- Ensure the render graph is valid without the imgui pass (the runtime already renders the game; imgui was an overlay).

**Verify:** GameRuntime links WITHOUT imgui symbols (check the link/objs); runs and renders without creating `imgui.ini`; the editor (`App`) still shows its full imgui UI unchanged. Build App+GameRuntime+EngineTests clean; suite green.

---

## Sequencing & notes
- Suggested order: **R1** (most user-visible), **R2**, **R3**, then **R4** (most architectural) — but each is independently committable and reviewable.
- Every task: MSVC build green + suite green; GameRuntime must remain a working game runtime; the editor (`App`) behavior must not regress.
- Reuse existing patterns: `AETHERCORE_EDITOR_APP` gating + the `editor/`/`debug/` source filters (`src/app/CMakeLists.txt`) already exclude editor code from the runtime — extend the same discipline to imgui/Aftermath.
- After all four land: push the whole `feature/shader-asset-pipeline` branch to a remote branch for review.
