# UI System Roadmap

**Architecture: Dear ImGui for debug/developer UI + ECS UI system for game UI.**

---

## DONE - Dear ImGui Integration

- [x] `CMake/Dependencies.cmake` - ImGui v1.91.8 via CPM (core + imgui_impl_glfw only)
- [x] `src/engine/CMakeLists.txt` - links imgui to Engine
- [x] `shaders/imgui.slang` - BDA vertex pull, bindless texture sample, R8G8B8A8 colour unpack
- [x] `src/engine/ImGuiRenderer.hpp/.cpp` - custom Vulkan backend
  - Double-buffered snapshot: game thread copies ImDrawData after ImGui::Render()
  - Render thread reads snapshot - no ImGui state access on render thread
  - Font texture uploaded via one-shot cmd buffer, registered in bindless set
  - Per-slot mapped GPU vertex + index buffers (grown lazily)
  - RenderGraph pass: LOAD_OP_LOAD → swapchain, registered after UIRenderer
  - Per-draw scissor via vkCmdSetScissor
- [x] `src/app/Application.hpp/.cpp` - ImGuiRenderer owned + wired up (BeginFrame/SnapshotFrame/SetWriteSlot)
- [x] `src/app/layers/DebugLayer.hpp/.cpp` - fully rewritten with ImGui (no entities, no PanelBuilder)

## DONE - ECS UI System (game UI foundation)

- [x] `UiComponents.hpp` - all ECS component types
- [x] `UiWorld.hpp/.cpp` - separate entt registry with +1 entity offset
- [x] `UiContext.hpp` - per-frame hot/active/drag state
- [x] `UiSystem.hpp/.cpp` - hit-test, drag, widget state flush; auto-BringToFront on click
- [x] `UiTheme.hpp` - swappable styling struct with Default() singleton
- [x] `UiWidgets.hpp/.cpp` - DrawButton, DrawSlider, DrawCheckbox, DrawProgressBar, DrawPanel; ApplyLayout/RunLayouts; all Spawn* helpers
- [x] QuadRenderer - DrawTexturedRect, clip rect (CPU AABB), bindless descriptor set bound every pass
- [x] UIRenderer - DrawTexturedRect, PushClipRect/PopClipRect
- [x] `ui_shapes.slang` / `ui_build_draws.slang` - kShapeTexturedRect, bindless sampling

---

## DONE - Layer Migration & Cleanup

- [x] Migrate `SandboxLayer` to ImGui - `ImGui::Columns` KV table, `SeparatorText` sections
- [x] Migrate `PhysicsLayer` to ImGui - same pattern
- [x] Migrate `FishingLayer` to ImGui - same pattern
- [x] Migrate `VoxelWorldLayer` to ImGui - same pattern
- [x] **Remove old UI system**: deleted `OverlayStyle.hpp`, removed all `PanelBuilder` / `UIRenderer` / `UiLayout` includes from migrated layers

## TODO - ECS UI (game UI features)

- [ ] Panel collapse: suppress input for children when parent is collapsed
- [ ] Panel resize: drag corner handle
- [ ] GPU scissor rects for scrollable panel content
- [ ] Scrollable panel body (scroll offset + clip rect + GPU scissor)
- [ ] Text input widget
- [ ] Dropdown / combo box (popup layer)
- [ ] Nine-slice rendering for scalable backgrounds
- [ ] Hover/press/focus color transitions (~80 ms lerp)
- [ ] Anchor presets (TopLeft, Center, BottomRight helpers)

## TODO - MMO Style UI Features
- [ ] UI layout system: auto-arrange children in rows/columns with padding
- [ ] UI layout system: auto-size panels to fit children (with optional max size + scrollbars)
- [ ] UI layout system: support for nested layouts (e.g. row of 3 columns, each with vertical stacks inside)
- [ ] UI layout system: support for dynamic content (e.g. inventory panel with variable number of item slots)
- [ ] UI layout system: support for flexible spacers (e.g. horizontal spacer that pushes siblings apart to fill available space)
- [ ] Think like inventory grids, skill bars, chat windows, etc. - what layout features would make building these easier?

## TODO - ImGui Enhancements

- [x] ImGui docking layout (ImGuiConfigFlags_DockingEnable)
- [x] Upgraded to v1.92.7-docking branch
- [x] Scissor rect clamped correctly (imgui_impl_vulkan handles this natively)
- [ ] Per-window font scaling
- [ ] Custom ImGui style matching engine's dark theme
- [x] Multi-viewport OS window tearoff - needs a dedicated VkQueue (second graphics queue from VulkanContext) to avoid concurrent vkQueueSubmit from game thread + render thread on the same handle. Docking works fully without it.
