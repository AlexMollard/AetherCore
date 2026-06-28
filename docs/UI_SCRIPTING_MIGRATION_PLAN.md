# UI and Scripting Migration Plan

This is the handoff plan for replacing AetherCore's current custom UI and daScript stack.

## Current Audit - 2026-06-28

Recent work moved debug tooling to Dear ImGui and split `DebugLayer` into dedicated `src/app/debug/*Panel` classes. The old custom screen-space UI stack has now been deleted.

Current state:

- `DebugLayer` is ImGui-based and no longer depends on `src/engine/ui`.
- The day/night sandbox controls have moved from `resources/scripts/sandbox.das` into the ImGui `Day / Night` debug panel.
- `Application.cpp` no longer fetches `UIRenderer` or drives the old UI begin/render/end flow.
- `AetherCore.cpp` no longer registers, initializes, or shuts down `UISubsystem`, `UIRenderer`, or `ui::UiSystem`.
- `RenderingSubsystem.cpp` no longer re-registers old UI render targets when render targets are rebuilt.
- `LoadingLayer` and the unused `loading` daScript module have been deleted.
- `resources/scripts/sandbox.das` no longer uses `require ui` or any `ui_*` APIs; its former sandbox panels were deleted and useful actions are keyboard-driven.
- Unused experimental `.das` scripts not required by sandbox were deleted, including `game.das`, `npc.das`, `player.das`, and unused scripts under `resources/scripts/systems/`.
- `src/app/scripting/modules/UIModule.cpp`, `src/engine/ui`, and the UI-only `src/engine/text` renderer have been deleted.
- Phase 3 preparation has started by trimming daScript down to the current sandbox surface.
- Existing docs were rewritten to describe ImGui tooling and the deleted custom UI stack.

## Decision

Adopt this order:

1. Dear ImGui for debug, tooling, and editor-style UI.
2. C# for runtime/gameplay scripting.
3. NoesisGUI for in-game/runtime UI.

The old custom UI system and daScript integration should be completely removed. Do not keep long-term compatibility layers for either system.

## Non-Negotiables

- Delete the old custom UI system rather than extending it.
- Delete daScript rather than wrapping it or preserving script compatibility.
- Keep the 3D debug renderer. It is not part of the old screen-space UI system.
- Keep the engine usable between phases where possible.
- Avoid designing Noesis bindings against daScript or old UI concepts.

## Keep

These systems are diagnostics/rendering infrastructure and should survive the UI deletion:

- `src/engine/physics/PhysicsDebugRenderer.hpp`
- `src/engine/physics/PhysicsDebugRenderer.cpp`
- 3D debug line/shape APIs
- pending debug vertices exposed through `AetherCore`
- physics debug toggles, if re-exposed through ImGui/C#
- light gizmos and other world-space diagnostic geometry
- debug shaders such as `shaders/debug_vert.slang` and `shaders/debug_frag.slang`
- render-graph/pass plumbing needed to draw 3D debug primitives

## Delete

The old screen-space UI stack should be removed:

- `src/engine/ui/UiWidgets.hpp`
- `src/engine/ui/UiWidgets.cpp`
- `src/engine/ui/UiTheme.hpp`
- `src/engine/ui/UiSystem.hpp`
- `src/engine/ui/UiSystem.cpp`
- `src/engine/ui/UISubsystem.hpp`
- `src/engine/ui/UISubsystem.cpp`
- `src/engine/ui/UIRenderer.hpp`
- `src/engine/ui/UIRenderer.cpp`
- `src/engine/ui/UiLayout.hpp`
- `src/engine/ui/UiContext.hpp`
- `src/engine/ui/UiComponents.hpp`
- `src/engine/ui/QuadRenderer.hpp`
- `src/engine/ui/QuadRenderer.cpp`

Likely delete these if no non-UI feature still needs them:

- `src/engine/text/TextRenderer.hpp`
- `src/engine/text/TextRenderer.cpp`
- `src/engine/text/FontAtlas.hpp`
- `src/engine/text/FontAtlas.cpp`

Delete old script-facing UI:

- `src/app/scripting/modules/UIModule.cpp`
- all `ui_*` daScript APIs
- all daScript bindings for UI components, panels, buttons, sliders, layout, text input, and graph widgets

Delete daScript in the C# phase:

- daScript subsystem code
- daScript modules
- daScript resource scripts under `resources/scripts/*.das` and `resources/scripts/systems/*.das`, once C# replacements exist
- daScript CMake/dependency wiring
- daScript asset/script loading paths

## Phase 1: ImGui Tooling Cutover

Goal: replace all debug/tooling UI with Dear ImGui and make old UI deletable.

Tasks:

- [x] Add Dear ImGui dependency and Vulkan/platform backend integration.
- [x] Add an `ImguiSubsystem` or equivalent engine-owned integration point.
- [x] Feed input into ImGui from the platform/input layer.
- [x] Render ImGui after world rendering, 3D debug geometry, and runtime UI.
- [x] Port `DebugLayer` from old `UIRenderer`/`UiWidgets` to ImGui.
- [x] Replace debug panels with ImGui windows, tables, trees, checkboxes, sliders, combo boxes, and plots.
- [x] Re-expose existing debug toggles through ImGui:
   - physics debug rendering
   - light gizmos
   - forward/render-queue debug flags
   - tonemap and FXAA controls
   - render graph stats
   - scene/entity inspector
- [x] Move day/night cycle controls from the old sandbox UI into an ImGui debug panel.
- [x] Remove `DebugLayer` dependencies on:
   - `ui/UIRenderer.hpp`
   - `ui/UiComponents.hpp`
   - `ui/UiLayout.hpp`
   - `ui/UiTheme.hpp`
   - `ui/UiWidgets.hpp`
- [x] Replace `LayerStack::GuiAll` naming/semantics with ImGui-specific layer submission.
- [x] Remove `Application.cpp` fetches of `UIRenderer`.
- [x] Remove `AetherCore.cpp` registration of `UISubsystem` and `UIRenderer`.
- [x] Delete `LoadingLayer` and the unused `loading` daScript module.
- [x] Remove `RenderingSubsystem.cpp` old UI target re-registration.

Progress notes:

- Dear ImGui uses the docking release tag `v1.92.8-docking`.
- `ImguiSubsystem` owns the ImGui context and GLFW/Vulkan backend lifetime.
- ImGui frames are submitted on the game thread, deep-copied into `RenderFramePacket`, and rendered on the render thread after render-graph execution.
- `DebugLayer` now submits ImGui tabs for performance, render controls/stats, debug toggles, camera info, scene/entity inspection, and script error toasts; its old `ui/...` dependencies are gone.
- The scene can render into an offscreen texture shown in an ImGui `Viewport` window, with mouse coordinates remapped into scene pixels and viewport image drags routed to camera/game input instead of ImGui window movement.
- The ImGui tooling exposes scene viewport render-resolution presets/custom size, aspect/display modes, viewport overlays, and a texture inspector backed by the GPU resource registry for previewing sampled color textures and metadata, including bindless slots.
- Scene viewport resize/rebuild handling now retires render queues before render-graph reset and registers debug/runtime UI passes with the offscreen scene extent, so custom viewport sizes do not leave stale queue slots or swapchain-sized render areas writing into scene textures.
- Scene viewport setting changes are requested from the game/UI thread and committed by the render thread at a frame boundary; invalid swapchain frames explicitly retire pending draw queues because their render-graph consumers are intentionally skipped.
- The shared render-graph rebuild path re-registers lighting compute passes, so editor viewport resolution changes and full swapchain rebuilds restore the same lighting pass graph.
- `DayNightPanel` in `src/app/debug/` now owns cycle enabled/manual mode, time-of-day, time speed, sun direction readout, and quick noon/midnight/sunrise actions. The duplicate old `Day / Night` panel and its widget polling were removed from `resources/scripts/sandbox.das`.
- `LoadingLayer` was removed entirely because the temporary loading overlay was not functional. `main.cpp` no longer pushes it, `SceneContext` no longer stores a loading overlay pointer, and the unused `LoadingModule.cpp` binding was deleted.
- Layer debug/tooling submission is now named for ImGui: `AppLayer::OnImGui()` and `LayerStack::ImGuiAll()`.
- `resources/scripts/sandbox.das` no longer depends on the old UI module. The old scene controls, entity inspector, and physics toy panels were removed; fox spawning, rotation toggling/speed, and toy spawning remain available through input actions.
- The old custom UI runtime path has been deleted from `Application.cpp`, `AetherCore.cpp`, and `RenderingSubsystem.cpp`.
- Unused experimental daScript files outside the sandbox dependency graph were deleted. The remaining sandbox script dependency graph is `sandbox.das`, `input_bindings.das`, `keycodes.das`, `systems/character_controller.das`, `systems/fox_system.das`, and `systems/third_person_camera.das`.
- The old script-facing UI binding (`UIModule.cpp`), `src/engine/ui`, and the UI-only text renderer under `src/engine/text` were deleted.

Exit criteria:

- Debug UI is entirely ImGui.
- App builds without `DebugLayer` depending on `src/engine/ui`.
- 3D debug geometry still renders.
- Old UI is unused by tooling.

## Phase 2: Delete Old UI

Goal: remove the custom UI system completely.

Tasks:

1. [x] Replace or delete remaining old UI use in `resources/scripts/sandbox.das`.
2. [x] Replace or delete runtime UI use in `resources/scripts/game.das` and `resources/scripts/systems/dialogue.das`.
3. [x] Remove `Application.cpp` old UI frame flow (`UIRenderer`, `ui::UiSystem`, hit-test/render/end calls).
4. [x] Delete `LoadingLayer` and the unused `loading` daScript module.
5. [x] Remove `AetherCore.cpp` old UI service registration/init/shutdown.
6. [x] Remove `RenderingSubsystem.cpp` old UI render-target reset hook.
7. [x] Delete `src/app/scripting/modules/UIModule.cpp` and remove module registration.
8. [x] Delete `src/engine/ui`.
9. [x] Delete old UI shaders and compute passes if they are only used by `QuadRenderer`, including `ui_build_draws` and old UI shape shaders.
10. [x] Delete old UI CMake source entries.
11. [x] Delete old UI docs or rewrite them to point at ImGui/Noesis.
12. [x] Delete text rendering only if it has no surviving non-UI use.

Exit criteria:

- No `#include "ui/..."`
- No `UISubsystem`
- No `UIRenderer`
- No `UiComponents`
- No `QuadRenderer`
- No old UI pass/shader references

## Phase 3: C# Runtime Cutover

Goal: replace daScript with C# before Noesis runtime UI is built.

Tasks:

1. [x] Choose hosting model: CoreCLR hosting for modern .NET integration.
2. Add a `CSharpScriptingSubsystem`.
3. Define engine-to-C# boundaries:
   - entity IDs and handles
   - scene/world access
   - components
   - input
   - physics controls
   - animation controls
   - asset references
   - logging
4. Port needed daScript gameplay scripts to C#.
5. Replace daScript modules with C# bindings.
6. Remove daScript module registration.
7. Remove daScript resource loading and script compile paths.
8. Remove `.das` scripts after C# equivalents exist.
9. Remove daScript dependency from CMake/CPM.

Progress notes:

- CoreCLR is the chosen C# hosting model. Do not introduce Mono unless CoreCLR embedding is proven infeasible.
- The remaining daScript runtime is intentionally narrowed to the live sandbox script graph while C# replacements are planned.
- Removed unused daScript modules with no remaining sandbox consumers:
   - `src/app/scripting/modules/DataModule.cpp`
   - `src/app/scripting/modules/GameComponentsModule.cpp`
   - `src/app/scripting/modules/SystemsModule.cpp`
- Removed the script-only `SystemFactory` service path and the unused experimental `src/app/components/GameComponents.hpp`.
- Sandbox still depends on daScript modules for world, renderer, camera, physics, effects, input, math, and animation.

Important:

- Do not mirror every daScript API blindly.
- Treat C# as a fresh public scripting API.
- Keep the API small until real gameplay scripts force expansion.

Exit criteria:

- No daScript dependency.
- No `.das` runtime dependency.
- Gameplay scripts run through C#.
- Debug/tooling UI still works through ImGui.

## Phase 4: NoesisGUI Runtime UI

Goal: add proper in-game UI after C# is in place.

Tasks:

1. Add NoesisGUI SDK integration.
2. Add a `NoesisSubsystem`.
3. Add Noesis resource providers for engine VFS/resources.
4. Add XAML/resource layout:
   - `resources/ui/screens/`
   - `resources/ui/styles/`
   - `resources/ui/fonts/`
5. Add Noesis input bridge from the platform/input layer.
6. Add Vulkan render backend integration for Noesis.
7. Add screen/view lifecycle:
   - load screen
   - show/hide
   - focus
   - update
   - render
   - unload
8. Bind Noesis to C# view models/events.
9. Replace temporary loading UI with Noesis.
10. Build initial runtime screens:
   - loading screen
   - pause/menu shell
   - HUD shell
   - settings shell

Rendering order:

1. world rendering
2. 3D debug geometry
3. Noesis runtime UI
4. ImGui debug/tooling overlay

Exit criteria:

- Runtime UI uses Noesis.
- Debug/tooling UI uses ImGui.
- Scripting-facing UI glue goes through C#, not daScript.
- Old UI remains deleted.

## Suggested New Modules

Possible folders:

- `src/engine/imgui/`
- `src/engine/noesis/`
- `src/app/scripting/csharp/`

Possible subsystem names:

- `ImguiSubsystem`
- `NoesisSubsystem`
- `CSharpScriptingSubsystem`

Keep each subsystem independent. ImGui should not depend on Noesis. Noesis should not depend on ImGui. C# may drive Noesis view models later, but the render/input backends should stay native-engine owned.

## Files To Audit First

Start with these files when implementing:

- `src/engine/AetherCore.cpp`
- `src/engine/AetherCore.hpp`
- `src/app/Application.cpp`
- `src/app/layers/DebugLayer.cpp`
- `src/app/layers/DebugLayer.hpp`
- `src/app/layers/LayerStack.cpp`
- `src/app/layers/LayerStack.hpp`
- `src/app/scripting/ScriptingSubsystem.cpp`
- `src/app/scripting/modules/UIModule.cpp`
- root `CMakeLists.txt`
- any nested CMake files that list UI/script sources

## Validation Checklist

After file deletions or CMake changes:

1. Run `/sync-lsp`.
2. Build with the default preset:
   - `cmake --preset default`
   - `cmake --build --preset default`
3. Build with clang if touching portability-sensitive code:
   - `cmake --preset vs2022-clang`
   - `cmake --build --preset vs2022-clang`
4. Confirm no old UI symbols remain:
   - `UISubsystem`
   - `UIRenderer`
   - `UiSystem`
   - `UiComponents`
   - `UiLayout`
   - `UiWidgets`
   - `QuadRenderer`
   - `UIModule`
5. Confirm debug renderer symbols remain:
   - `PhysicsDebugRenderer`
   - `AddDebugLine`
   - `AddDebugAabb`
   - `AddDebugBox`
   - `AddDebugSphere`
   - `DrawImmediateDebugPrimitives`

## Final Target State

AetherCore should end in this shape:

- ImGui handles engine/editor/debug/tooling UI.
- C# handles gameplay/runtime scripting.
- NoesisGUI handles in-game UI.
- No daScript remains.
- No old custom UI remains.
- 3D debug rendering remains available and separate from UI.
