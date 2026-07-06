# DPI Support (per-monitor, ImGui editor + 3D viewport) - Design

Date: 2026-07-06
Status: Approved (design). Ready for implementation planning. Lands on the
`claude/imgui-multiviewport` branch (DPI pairs with multi-viewport: it matters most when
a panel is torn out onto a hi-DPI second monitor, and is verified together).

## Goal

Make the AetherCore editor render crisply and at correct size on high-DPI and mixed-DPI
multi-monitor setups: sharp fonts, appropriately-scaled UI, torn-out viewports that scale
to whatever monitor they land on, a user-adjustable UI-scale knob, and a DPI-aware 1:1
scene-viewport resolution mode. The 3D scene already renders at physical framebuffer
resolution; this focuses on the Dear ImGui tooling UI plus one scene-viewport addition.

## Decisions (locked with the user)

- **Automatic per-monitor DPI** for the ImGui UI (not a single global scale).
- **Manual UI-scale setting** in addition to auto DPI (`graphics.uiScale`).
- **3D viewport:** add a DPI-aware **"Match Panel"** scene-resolution mode (the existing
  3D path is otherwise already DPI-robust).
- **Approach:** ImGui 1.92's native DPI model (`io.ConfigDpiScaleFonts` +
  `io.ConfigDpiScaleViewports`), not the pre-1.92 manual font-reload path.

## Background: current state (verified)

- **No DPI handling exists** in engine code (grep-clean). `Window` creates a plain GLFW
  window; `ImguiSubsystem` loads Roboto at a fixed 15 px.
- **OS awareness is already handled:** GLFW **3.4** (`CMake/Dependencies.cmake`) makes the
  Windows process **per-monitor-DPI-aware v2** at `glfwInit` (no manifest overrides it), so
  `glfwGetFramebufferSize` reports physical pixels and the swapchain already renders at
  physical resolution.
- **`imgui_impl_glfw` already feeds DPI:** it populates `ImGuiPlatformMonitor::DpiScale`
  from `glfwGetMonitorContentScale` and sets `io.DisplaySize` (logical) +
  `io.DisplayFramebufferScale` (content scale) each `NewFrame`.
- **ImGui 1.92 DPI model (confirmed in imgui.cpp):**
  - `io.ConfigDpiScaleFonts` -> sets `style.FontScaleDpi = CurrentDpiScale`
    (imgui.cpp:16701). Fonts re-rasterize crisply via the 1.92 dynamic-font system; **no
    manual font reload**. Scales **fonts only**.
  - `io.ConfigDpiScaleViewports` -> `ScaleWindowsInViewport(...)` on a monitor-DPI change
    (imgui.cpp:17028). Scales **window geometry** (ImGui window pos/size) only — complements
    `ConfigDpiScaleFonts`, no double font scaling.
  - `GetFontSize() == FontSizeBase * FontScaleMain * FontScaleDpi` (imgui.cpp:9374). So a
    manual `FontScaleMain` composes with auto DPI.
  - Nuance: `ScaleWindowsInViewport` scales on the DPI **delta**; on the very first frame a
    viewport's `DpiScale` is 0 so it is not scaled (imgui.cpp:17025 guard). Fixed style
    paddings therefore aren't auto-scaled for the **initial** monitor (font-derived widget
    sizes are). Handled below.
- **Custom `ImguiViewportRenderer`** (multi-viewport) already sizes secondary swapchains
  from each viewport's own `DrawData->FramebufferScale` (fixed in the multi-viewport review),
  so torn-out windows already pick up per-monitor DPI.
- **3D scene viewport:** `RenderingSubsystem::ResolveRequestedSceneViewportExtent`
  (RenderingSubsystem.cpp:61) resolves the render resolution from an explicit
  `SceneViewportResolutionMode` (Fixed720p/1080p/1440p, Custom, WindowNative=swapchain
  physical) — **decoupled from the panel's logical size**, so no hidden logical-vs-physical
  blur. The `ViewportPanel` displays the texture via ImGui (DPI-crisp projection) and maps
  the mouse by ratio (`Input::SetMouseViewportTransform`, DPI-agnostic).

## The one manual override to remove

`ImguiSubsystem::Init` and `BeginFrame` set `io.DisplaySize = window->GetFramebufferSize()`
(physical pixels). `imgui_impl_glfw`'s `NewFrame` overwrites this with the correct DPI-aware
`DisplaySize` (logical) + `DisplayFramebufferScale`, but the manual set forces *physical* as
*logical* and is wrong wherever it is actually consumed (pre-backend frames). Remove it and
let `imgui_impl_glfw` own `DisplaySize`/`DisplayFramebufferScale`.

## Design

### A. Automatic per-monitor DPI (ImGui UI)
- In `ImguiSubsystem::Init`, set `io.ConfigDpiScaleFonts = true` and
  `io.ConfigDpiScaleViewports = true`.
- Remove the manual `io.DisplaySize = GetFramebufferSize()` assignments (Init + BeginFrame).
- **Font lifetime:** the dynamic re-rasterizer needs the TTF data alive. `m_fontData` /
  `m_iconFontData` are already retained members with `FontDataOwnedByAtlas = false` — keep
  that; confirm the merged Font Awesome range re-rasterizes with the base font.
- **Sizes/padding (low-risk primary path):** rely on the flags. Fonts scale per-monitor
  (`FontScaleDpi`); window geometry scales per-monitor (`ConfigDpiScaleViewports`); most
  widget dimensions are font-derived (`GetFrameHeight() == FontSize + FramePadding.y*2`) so
  they follow the font. `style.Sizes` (fixed padding/spacing) is global in ImGui and not
  auto-scaled per-monitor — accepted (matches ImGui's own current behavior). See the tune
  option below if the initial-monitor padding looks off on hardware.
- No change needed in `ImguiViewportRenderer` (already per-viewport DPI-aware).

### B. Manual UI-scale setting
- Add `float uiScale = 1.0f;` to `EngineSettings::Graphics` + one `ForEachSettingField`
  line (`graphics.uiScale`). Clamp to `[0.5, 3.0]` in `Sanitize`.
- `ImguiSubsystem` gains `SetUiScale(float)` -> `style.FontScaleMain = uiScale`
  (producer-thread only). `FontScaleMain` composes with the per-window `FontScaleDpi`
  (`GetFontSize = FontSizeBase * FontScaleMain * FontScaleDpi`), so the manual knob and DPI
  multiply cleanly; font-derived widget heights follow.
- `SettingsService::ApplyLive("graphics.uiScale")` -> `engine->SetUiScale(...)`, and an
  `AetherCore::SetUiScale` forwarder (mirrors the `imguiViewports` toggle). The
  `SettingsPanel` field appears automatically (float -> `InputFloat`).

**Optional padding tune (only if hardware shows it's needed):** to also scale fixed
`style.Sizes` by the manual `uiScale` (and/or the primary-monitor DPI), stash the unscaled
`ImGuiStyle` after `ApplyTheme` and, on change, rebuild `style = base;
style.ScaleAllSizes(factor)` before setting `FontScaleMain`. `ScaleAllSizes` touches
`style.Sizes` only (not window rects), so it does not conflict with
`ConfigDpiScaleViewports`. Kept out of the primary path to avoid over-scaling; decided
visually on hi-DPI hardware.

### C. 3D viewport: "Match Panel" resolution mode
- Add `SceneViewportResolutionMode::MatchPanel`. The `ViewportPanel` already computes its
  logical `available` size; pass the panel's **physical** size (logical ×
  `ImGui::GetIO().DisplayFramebufferScale` / the panel viewport's DpiScale) as a requested
  extent to `RenderingSubsystem`.
- `ResolveRequestedSceneViewportExtent` returns that clamped physical extent for
  `MatchPanel`, so the scene renders 1:1 at the panel's true pixel density and follows both
  resize and monitor DPI. Reuses the existing pending-rebuild/commit machinery
  (`IsSceneViewportRebuildPending` / `CommitPendingSceneViewportSettings`).

## File-by-file change list

- `src/engine/utils/EngineSettings.hpp` - `graphics.uiScale` field + reflection line;
  clamp in `Sanitize`.
- `src/engine/utils/SettingsService.cpp` - `graphics.uiScale` live-apply case.
- `src/engine/AetherCore.hpp/.cpp` - `SetUiScale` forwarder.
- `src/engine/imgui/ImguiSubsystem.hpp/.cpp` - enable the two config flags; remove manual
  `DisplaySize`; base-style stash + `ApplyUiScale()`; `SetUiScale`.
- `src/engine/rendering/RenderingSubsystem.hpp/.cpp` - `SceneViewportResolutionMode::MatchPanel`
  + resolve to the requested physical panel extent.
- `src/app/debug/ViewportPanel.cpp` - request the panel's physical extent when MatchPanel;
  add the mode to the resolution combo.
- No engine `CMakeLists.txt` edit (GLOB_RECURSE).

## Verification

The decisive test is **on real hi-DPI hardware** (the reason for this work):
- Build MSVC (`build-vs2022-msvc`, Debug); `EngineTests` green (settings round-trip covers
  `uiScale`).
- On a hi-DPI monitor (e.g. 150%/4K): fonts are crisp (not upscaled-blurry); UI is
  appropriately sized.
- Tear a panel out onto a **different-DPI** monitor -> it rescales to that monitor; drag it
  back -> rescales again. (Ties into the multi-viewport tear-out test.)
- `uiScale` slider makes the whole editor larger/smaller and composes with DPI.
- Viewport panel set to **Match Panel** -> 3D view is 1:1 crisp and tracks panel resize +
  monitor DPI.
- Vulkan validation clean.
- **Tune item (honest):** the fonts-vs-fixed-padding composition on the initial monitor is
  the one area to eyeball; if `ScaleAllSizes` double-scales against `ConfigDpiScaleViewports`
  window scaling, the fallback is to scale sizes only for the initial monitor and let the
  flag own subsequent transitions.

## Out of scope

- Scaling the 3D scene itself by DPI beyond the new Match Panel mode (other modes stay).
- Non-Windows DPI specifics (X11/Wayland content scale is handled by GLFW/imgui_impl_glfw;
  same code path, verify opportunistically).
- Per-widget DPI overrides / custom font-size-per-monitor beyond `FontScaleMain`.
