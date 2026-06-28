# `imgui/` - Debug and tooling UI

The old custom screen-space UI system under `src/engine/ui/` has been removed.

Current debug/tooling UI is handled by Dear ImGui:

| File | Role |
|---|---|
| `src/engine/imgui/ImguiSubsystem.hpp` / `.cpp` | Owns the ImGui context and GLFW/Vulkan backend lifetime. |
| `src/engine/imgui/ImguiFrameData.hpp` | Carries deep-copied ImGui draw data from the game thread to the render thread. |
| `src/app/debug/` | Dockable debug panels for viewport, rendering, scene inspection, day/night settings, and diagnostics. |

Runtime/gameplay UI is intentionally deferred to the Noesis phase in `docs/UI_SCRIPTING_MIGRATION_PLAN.md`.
