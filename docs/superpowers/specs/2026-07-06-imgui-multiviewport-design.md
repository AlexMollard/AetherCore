# Dear ImGui Multi-Viewport Support - Design

Date: 2026-07-06
Status: Approved (design, revised). Ready for implementation planning.

## Revision note

The first committed version of this spec (Approach A) assumed we could render
secondary viewports on the render thread by calling `ImGui_ImplVulkan_RenderDrawData`
on snapshotted draw data. Grounding the plan against the vendored backend disproved that:
`ImGui_ImplVulkan_RenderDrawData` requires the backend's internal per-viewport
`ImGui_ImplVulkan_ViewportData` (imgui_impl_vulkan.cpp:594), which only exists for
secondary viewports if the stock `Renderer_CreateWindow` hook runs - and that hook also
creates the swapchain via `vkDeviceWaitIdle`, which cannot run on the producer thread
concurrently with the render thread.

Two follow-up decisions were taken with the user:
1. Rejected running secondary viewports on the **producer thread** (stock backend,
   queue-mutex) because torn-out panels that sample **live engine images** - notably the
   3D **Viewport** (`post.GetFinalColorImageView()`, ViewportPanel.cpp:334) - would read
   render-thread-written images with no synchronization: a cross-thread GPU hazard.
2. Chose **full** tear-out (including the 3D scene onto a second monitor), which requires
   rendering secondary viewports on the **render thread** with a small custom ImGui
   renderer. This is the design below.

## Goal

Enable Dear ImGui **multi-viewport**: any tool/editor panel - including the live 3D
Viewport - can be dragged out of the main window into an independent OS window and
re-docked. Reuse AetherCore's existing systems (producer/consumer render split,
`ImguiFrameData` deep-copy snapshot, `RunExclusive` quiesce, `SettingsService`).

## Decisions (locked with the user)

- **Control model:** live runtime toggle via `SettingsService` (`graphics.imguiViewports`).
- **Default state:** ON.
- **Scope:** FULL tear-out, including live-image panels (3D Viewport, Texture Inspector).
- **Threading:** secondary viewports render on the **render thread** (correct GPU sync);
  GLFW platform-window management stays on the **producer/main thread**.
- **Renderer:** custom minimal ImGui renderer for secondary viewports, reusing the
  **public** `ImGui_ImplVulkanH_Window` swapchain helpers + the backend's texture
  descriptors (`ImGui_ImplVulkan_AddTexture`) + ImGui's own embedded shaders. We do **not**
  register the stock Vulkan renderer viewport hooks and never call
  `RenderPlatformWindowsDefault`.
- **Structural viewport destroy:** reuse the existing `RunExclusive` quiesce. Create and
  resize need no quiesce (render-thread-local). DPI scaling and a deferred-retire queue
  are out of scope.

## Background: current architecture

- **Windowing** - `Window` ([Window.hpp](../../../src/engine/platform/Window.hpp)) wraps
  one GLFW window; `PollEvents()` on the producer thread.
- **Swapchain** - `Swapchain` ([Swapchain.hpp](../../../src/engine/vulkan/Swapchain.hpp)),
  VkBootstrap, main window; created on the main thread under `RunExclusive`; submitted/
  presented on the render thread.
- **ImGui** - stock `imgui_impl_glfw` + `imgui_impl_vulkan`, ImGui **v1.92.8-docking**,
  `UseDynamicRendering = true`
  ([ImguiSubsystem.cpp](../../../src/engine/imgui/ImguiSubsystem.cpp)). `BeginFrame` (NewFrame)
  and `CaptureFrame` (Render + deep-copy the **main** viewport's `ImDrawData`) on the
  producer; `RenderFrame` (main viewport) on the render thread.
- **ImguiFrameData** ([ImguiFrameData.cpp](../../../src/engine/imgui/ImguiFrameData.cpp)) -
  thread-transferable deep copy (clones each `ImDrawList` via `CloneOutput`). Captures one
  viewport today.
- **Frame loop / RunExclusive** ([AetherCore.cpp](../../../src/engine/AetherCore.cpp)) -
  producer submits `RenderFramePacket`s; render thread `ExecuteRenderFrame` ->
  `EndFrame` (render graph, `imgui->RenderFrame`, `SubmitAndPresent`). `RunExclusive`
  parks the render thread + idles the GPU for structural GPU mutation.
- **Settings** - `EngineSettings` + `ForEachSettingField` (add a setting = one struct field
  + one `f(...)` line; [EngineSettings.hpp](../../../src/engine/utils/EngineSettings.hpp));
  `SettingsService::ApplyLive` dispatches live effects
  ([SettingsService.cpp](../../../src/engine/utils/SettingsService.cpp)); the `SettingsPanel`
  UI auto-generates from the reflection list. Engine sub-libs use `GLOB_RECURSE
  CONFIGURE_DEPENDS`, so new engine `.cpp`/`.hpp` auto-register (no CMake edit).
- **Editor shell** ([DebugLayer.cpp](../../../src/app/layers/DebugLayer.cpp)) - dockspace host,
  `PassthruCentralNode`, chrome gated to frame >= 1 (frame-0 crash mitigation). The app
  already depends on `imgui_internal.h` (DockBuilder).

## The core problem

ImGui multi-viewport splits across two threads by this engine's invariants:
- `UpdatePlatformWindows()` (GLFW window create/destroy/move/resize) must be on the
  producer thread (GLFW + `PollEvents`).
- Secondary-viewport GPU work (swapchain, submit, present) must be on the render thread,
  serialized with the main swapchain **and** correctly synchronized with engine image
  writes (the 3D Viewport samples a render-thread-written image).

Key backend facts that shape the design:
- `ImGui_ImplVulkan_RenderDrawData` needs the internal per-viewport `ViewportData`
  (imgui_impl_vulkan.cpp:594) - unusable for secondary viewports without the stock renderer
  hook. The **main** viewport's `ViewportData` is created by `Init` (imgui_impl_vulkan.cpp:1428),
  which is why today's single-viewport path works.
- `ImGui_ImplVulkanH_Window` and its lifecycle helpers **are public**
  (imgui_impl_vulkan.h:207-280) - so we can own per-viewport swapchains directly.
- Texture descriptors are shared context-global state: `ImGui_ImplVulkan_AddTexture`
  returns a `VkDescriptorSet` (the `ImTextureID`), and texture uploads are processed by
  the first `RenderDrawData` of the frame (the main viewport's `RenderFrame`, on the render
  thread) - so secondary rendering only needs to **bind** already-created descriptors.

## The linchpin

Register only the **glfw platform** viewport hooks; after `ImGui_ImplVulkan_Init`, null the
Vulkan **renderer** viewport hooks (`Renderer_CreateWindow/DestroyWindow/SetWindowSize/
RenderWindow/SwapBuffers`) and never call `RenderPlatformWindowsDefault`. ImGui null-guards
each renderer hook, so `UpdatePlatformWindows()` becomes pure GLFW window management (no
surface, no swapchain, no `vkDeviceWaitIdle`) and is safe on the producer thread. The
render thread owns all secondary GPU work via a custom renderer.

## Design

### Data flow (per frame)

Producer thread (replaces the single `CaptureFrame` call in `RunFrameLoop`):
1. `imgui->Render()` - `ImGui::Render()`; refresh `WantsInputCapture`. Produces main +
   each secondary viewport's `DrawData`.
2. Structural-destroy check (see Lifetime). If a secondary window will be destroyed this
   frame, wrap step 3 + renderer retire in `RunExclusive(Drain, ...)`.
3. `imgui->UpdatePlatformWindows()` - GLFW only (renderer hooks nulled); new windows get a
   valid `PlatformHandle`. Gated to frame >= 1.
4. `imgui->SnapshotFrame(outFrame)` - deep-copy main + every secondary viewport (draw data
   + pos/size/fb-scale + GLFW `PlatformHandle` + `ImGuiID`).
5. submit packet.

Render thread (`EndFrame`, after the main-viewport `RenderFrame` + main present):
6. `imgui->RenderViewports(packet.imgui)` -> `ImguiViewportRenderer`: reconcile per-viewport
   swapchains, render each secondary's draw data, present. Because this runs after the
   render graph wrote the final-color image (same thread, ordered, barriered), a torn-out
   Viewport samples it safely - identical synchronization to the docked case.

### Components

**Changed - `ImguiFrameData`** (N-viewport snapshot)
- `struct CapturedViewport { ImGuiID id; ImDrawData draw; std::vector<ImDrawList*> owned;
  ImVec2 pos, size, fbScale; void* platformHandle; };`
- Keep the existing main-viewport members; add `std::vector<CapturedViewport>
  m_secondary;`. Factor the `CloneOutput` loop into a helper used for main + each secondary.
- The **main** viewport stays a dedicated member consumed unchanged by `RenderFrame`; the
  secondary vector is consumed only by `RenderViewports`. No overlap.

**New - `ImguiViewportRenderer`** (`src/engine/imgui/`, render-thread-owned)
- One shared VkPipeline + VkPipelineLayout + VkDescriptorSetLayout for ImGui, created from
  ImGui's own embedded SPIR-V (copied from imgui_impl_vulkan.cpp) with a set-0 layout
  **identically defined** to the backend's (binding 0 = combined image sampler, fragment
  stage) so `AddTexture` descriptor sets bind, and a 16-byte vertex push-constant
  (scale+translate). Color format = the main swapchain format (dynamic rendering).
- `std::unordered_map<ImGuiID, PerViewport>` where `PerViewport` holds an
  `ImGui_ImplVulkanH_Window` (public) + its own vertex/index buffer ring.
- `Render(const ImguiFrameData&)`: per captured secondary viewport - lazily create surface
  (`glfwCreateWindowSurface` from `platformHandle`) + swapchain (`SelectSurfaceFormat`
  forced to the main format, `SelectPresentMode` FIFO, `CreateOrResizeWindow`); rebuild on
  size change / `OUT_OF_DATE`; acquire -> dynamic-rendering pass -> upload vtx/idx -> draw
  loop (bind pipeline, push scale/translate, per-cmd scissor + bind `pcmd->GetTexID()`
  descriptor set + `vkCmdDrawIndexed`) -> submit + present on the graphics queue. Adapted
  from the stock `ImGui_ImplVulkan_RenderWindow` (imgui_impl_vulkan.cpp:2162) and
  `ImGui_ImplVulkan_RenderDrawData` body (imgui_impl_vulkan.cpp:573-720), reading our owned
  snapshot instead of `viewport->DrawData`.
- `RetireViewports(const std::vector<ImGuiID>& departed)`: `DestroyWindow` + surface for
  gone viewports; called from the producer inside `RunExclusive` (render thread parked, GPU
  idle) so it is safe to touch the render-thread map, and it precedes the producer's
  `glfwDestroyWindow`.
- Depends only on `VulkanContext` + public `ImGui_ImplVulkanH_*` / `ImGui_ImplVulkan_*`.

**Changed - `ImguiSubsystem`**
- `InitBackends`: set `ViewportsEnable` before `ImGui_ImplVulkan_Init` so multi-viewport
  support installs, then null the five Vulkan renderer viewport hooks and set the flag to
  match the setting. Create the `ImguiViewportRenderer`. Apply viewport style
  (`WindowRounding = 0`, `Colors[ImGuiCol_WindowBg].w = 1.0f`) when enabled.
- Split `CaptureFrame` into `Render()`, `UpdatePlatformWindows()` (frame-gated),
  `SnapshotFrame(ImguiFrameData&)`; add `SecondaryViewportIdsWithPendingDestroy()` (uses
  `imgui_internal.h`: `ImGuiViewportP::LastFrameActive < g.FrameCount`).
- `RenderViewports(const ImguiFrameData&)` (render thread) -> `ImguiViewportRenderer`.
- `SetViewportsEnabled(bool)` (producer thread): set/clear the config flag + reapply style.
- Shutdown: GPU-idle, destroy the `ImguiViewportRenderer` (all secondary swapchains) before
  `ImGui_ImplVulkan_Shutdown`.
- `CaptureFrame`/`RenderFrame` main-viewport behavior otherwise unchanged.

**Changed - `AetherCore`**
- `RunFrameLoop`: replace `CaptureFrame` with Render -> (pending-destroy? quiesced
  UpdatePlatformWindows + `RetireViewports` : plain UpdatePlatformWindows) -> SnapshotFrame.
- `EndFrame`: after `m_imgui->RenderFrame(...)` + present, call
  `m_imgui->RenderViewports(packet.imgui)`.
- Add `SetImguiViewportsEnabled(bool)` forwarding to `m_imgui` (mirrors `SetVsync`).

**Changed - `EngineSettings` + `SettingsService`**
- Add `bool imguiViewports = true;` to `Graphics` + one `ForEachSettingField` line
  (`"graphics.imguiViewports"`). The `SettingsPanel` checkbox appears automatically.
- `ApplyLive("graphics.imguiViewports")` -> `engine->SetImguiViewportsEnabled(...)`. Applied
  on the producer thread (settings UI / startup `ApplyAll`).

### Lifetime & threading

- **Create** (tear-out): producer `glfwCreateWindow` via `UpdatePlatformWindows` (no GPU);
  render thread lazily creates the swapchain next `RenderViewports`. No quiesce.
- **Move/resize**: render-thread-local swapchain rebuild (`OUT_OF_DATE` / size change). No
  quiesce.
- **Destroy** (re-dock / close / toggle-off): detected before `UpdatePlatformWindows` via
  `LastFrameActive`. Wrap `UpdatePlatformWindows` + `RetireViewports(departed)` in
  `RunExclusive(Drain)` so the render thread is parked and the GPU idle when the swapchain/
  surface are destroyed, before the GLFW window is destroyed. `RunExclusive` remains used
  for its existing main-swapchain-recreate role too.

### Edge cases

- **Frame-0 crash:** gate viewport creation / `UpdatePlatformWindows` to frame >= 1.
- **`imgui.ini` persistence:** torn-out layout restores; the frame-1 gate makes it safe.
- **Secondary swapchain format:** forced to the main swapchain format so the shared pipeline
  applies.
- **Textures:** the main viewport's `RenderFrame` (render thread, runs before
  `RenderViewports`) processes the shared texture list, so secondary rendering only binds
  existing descriptors.
- **Shutdown ordering:** GPU idle -> destroy secondary swapchains -> `ImGui_ImplVulkan_Shutdown`
  -> `ImGui_ImplGlfw_Shutdown`.
- **Linux/Wayland:** viewports work; programmatic window positioning is a GLFW/Wayland
  limitation (X11 full). Documented caveat.

## File-by-file change list

- `src/engine/imgui/ImguiFrameData.hpp/.cpp` - N-viewport snapshot + clone helper.
- `src/engine/imgui/ImguiViewportRenderer.hpp/.cpp` - **new** render-thread renderer
  (pipeline + per-viewport swapchains + draw/present + retire).
- `src/engine/imgui/ImguiSubsystem.hpp/.cpp` - hook nulling, config flag + style, split
  capture, pending-destroy detection, `RenderViewports`, `SetViewportsEnabled`, shutdown.
- `src/engine/AetherCore.hpp/.cpp` - producer sequence + structural quiesce; `EndFrame`
  `RenderViewports`; `SetImguiViewportsEnabled`.
- `src/engine/utils/EngineSettings.hpp` - `imguiViewports` field + reflection line.
- `src/engine/utils/SettingsService.cpp` - live-apply case.
- `tests/utils/EngineSettingsTests.cpp` (+ `tests/CMakeLists.txt` if a new test file) -
  cover the new setting's default / apply / round-trip.
- No engine `CMakeLists.txt` edit (GLOB_RECURSE). Comments document the Wayland caveat.

## Verification

- Build **both** presets (`build-ninja-clang` clang-cl, `build-vs2022-msvc` MSVC).
- `EngineTests` (doctest) green, including the new settings cases.
- Run from the build-tree root (`shaders://` CWD rule). Manual matrix:
  - Tear a tool panel out -> OS window; re-dock; resize; drag across monitors.
  - **Tear the 3D Viewport out to a second monitor** -> live scene renders correctly, no
    tearing/garbage.
  - Toggle `graphics.imguiViewports` off (windows merge back) / on (tear out again).
  - Close with a panel torn out -> clean shutdown; restart with persisted layout -> spawns
    at frame >= 1.
- Vulkan validation layers clean (no queue/thread/lifetime/sync errors), including sync
  validation for the torn-out Viewport sampling the final-color image.
- Tracy: quiesce fires only on structural **destroy**, not per frame; producer stays up to
  3 frames ahead.
- Frame-0 crash does not regress.

## To confirm during planning (not design-affecting)

- Exact `io.IniFilename` location/policy.
- Whether `SettingsService` ever applies off the producer thread (else defer the flag flip
  one frame).
- `Input::IsMouseViewportInputActive()` / `io.MouseHoveredViewport` interaction.

## Out of scope

- Per-monitor DPI viewport scaling (`ImGuiConfigFlags_DpiEnableScaleViewports`).
- Deferred cross-thread window-retirement queue (only if `RunExclusive` hitches appear).
- Changes to the engine bindless/descriptor-heap system (ImGui uses its own pool).
