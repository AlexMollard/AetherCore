# Separate Launcher Window (on hardened multi-viewport)

**Date:** 2026-07-13
**Status:** SUPERSEDED by `2026-07-13-multiprocess-launcher-targets-design.md`.
Phase 1 (multi-viewport input — verified working) and Phase 2 (force-resize policy
deleted — shipped) landed. Phase 3 (launcher as an ImGui viewport window) was
abandoned: the viewport promotion would not create a separate OS window in this custom
renderer, so the launcher becomes a separate **process** instead.

## Problem

Launching the app into the project launcher produces inconsistent, visibly janky
window sizing — the OS window flips between ~1080p and ~1440p, often oscillating
("going in and out"), because the size is being *forced* every frame.

Root cause: `DebugLayer::UpdateWindowSizing` (`src/app/layers/DebugLayer.cpp:811`)
runs each frame and calls `Window::SetSize` on every launcher↔editor transition —
shrinking the OS window to a fixed `1920×1080` for the launcher
(`kLauncherWindowWidth/Height`, `src/app/debug/EditorProjectManager.hpp:25`) and
restoring the last editor size otherwise (`DebugLayer.cpp:840,844`). It then waits
12 frames to "settle" and re-captures the size (`CaptureEditorWindowSize`,
`DebugLayer.cpp:798`). This capture → force → restore loop oscillates when either:

- the `launcherMode` flag flickers (e.g. transient state during load), or
- the captured size returns DPI-adjusted — ImGui runs with
  `ConfigDpiScaleViewports = true` (`src/app/imgui/ImguiSubsystem.cpp:115`), so on a
  1440p-at-scaling monitor `GetWindowSize` disagrees with what was `SetSize`'d,
  producing the 1080↔1440 ping-pong.

Separately, the launcher is not its own window at all: it is a **fullscreen ImGui
panel** stretched over the main viewport
(`src/app/debug/ProjectLauncherWindow.cpp:196` — `SetNextWindowPos(WorkPos)` /
`SetNextWindowSize(WorkSize)`), so it can only ever be the size of the editor
window and cannot adopt its preferred ~1450px-wide layout.

## Goals

- The project launcher is its **own OS window**, fixed at ~1450px wide (its
  ImGui layout's sweet spot), independent of the editor window's size.
- Launcher and editor **coexist** as separate windows (Unity-Hub style): opening a
  project raises/focuses the editor window; closing or switching a project raises
  the launcher. Both windows stay alive.
- The forced per-frame window resizing is **removed entirely**; the editor window
  owns its own size and nothing drives it externally.
- Floated ImGui windows (the launcher, and dockable panels) route input correctly, so
  the launcher is fully interactive as a secondary viewport.

## Non-goals

- A second, hand-rolled Vulkan swapchain / second ImGui context. We reuse the
  existing multi-viewport rendering instead (see Approach).
- Reworking the launcher's visual content/layout beyond fixing its width.
- Any change to GameRuntime (it has no editor/launcher).

## Approach

The engine **already renders multiple OS windows**: ImGui runs with
`ImGuiConfigFlags_ViewportsEnable | DockingEnable`
(`src/app/imgui/ImguiSubsystem.cpp:106,110`) and a custom, threaded per-viewport
Vulkan path (`UpdatePlatformWindows`, `SnapshotFrame`'s secondary-viewport draw-data
capture at `ImguiSubsystem.cpp:309–324`, `SecondaryViewportIdsWithPendingDestroy`).
Dragging a panel out of the main window already spawns its own OS window through
this path.

Therefore the launcher becomes a **dedicated ImGui platform-viewport window** on top
of that existing infrastructure — no new swapchain code. The multi-viewport path is,
however, the subsystem flagged *built-but-runtime-unverified*, so the work is phased to
confirm (and only if needed, harden) that foundation before the launcher rides on it.
(Note: the console scroll bug once suspected to be a secondary-viewport input defect
turned out to be a separate ConsolePanel issue — multi-line log entries breaking its
uniform-height `ImGuiListClipper` — so it is **not** evidence of a viewport input bug.)

**Window/viewport mapping:** the boot GLFW window (the ImGui *main viewport*) is the
**editor**; the **launcher** is a **secondary viewport** window. This is the cleanest
fit because the main viewport is special (created at boot, never destroyed). At
startup with no project loaded, the editor main window shows a neutral/empty backdrop
while the launcher window sits on top and focused — an accepted consequence of this
mapping.

## Design — phased

Each phase builds and is verifiable on its own, de-risking the fragile foundation
before the launcher rework.

### Phase 1 — Verify (and only if needed, harden) multi-viewport input

Confirm input already routes correctly to secondary viewport windows before the
launcher relies on it. Float a panel into its own OS window and test
wheel/scroll/click/keyboard. If it works, Phase 1 is a quick sign-off and we proceed.
If a real defect surfaces, diagnose + fix it — the suspected area is the platform input
wiring for viewport windows (the engine suppresses the stock Vulkan *renderer* viewport
hooks at `ImguiSubsystem.cpp:636`; the GLFW *platform* input callbacks must remain
installed on each viewport window).

- **Note:** the earlier console scroll bug is not evidence here — it was a separate
  ConsolePanel `ImGuiListClipper` + multi-line-entry defect, unrelated to viewport input.
- **Verification:** float a panel out; confirm wheel, scrollbar, clicks/selection, and
  keyboard all work in it.

### Phase 2 — Delete the force-resize policy

Remove the external window-size forcing so the editor window owns its size:

- Delete `DebugLayer::UpdateWindowSizing`, `CaptureEditorWindowSize`, and their state
  (`m_editorWindowW/H`, `m_windowSettleFrames`, `m_launcherModeTracked`,
  `m_windowModeInit`) and the `UpdateWindowSizing(context)` call in
  `DebugLayer::OnImGui` (`DebugLayer.cpp:924`).
- Retire the launcher-drives-the-window use of `kLauncherWindowWidth/Height`; keep a
  launcher-size constant for Phase 3's fixed viewport size.
- Simplify `Application::ResolveEditorBootWindow`
  (`src/app/Application.cpp:48`): the main (editor) window always opens at the editor
  size (settings/last-saved), never the launcher size.
- Editor window-size persistence continues via `EditorState.toml`
  (`editor.window_width/height`), but captured on genuine window-resize/shutdown
  rather than the launcher dance.
- **Verification:** toggling launcher ↔ editor no longer resizes the OS window; the
  1080↔1440 oscillation is gone; the editor window keeps whatever size the user set.

### Phase 3 — Launcher as its own fixed-size viewport window

- In `ProjectLauncherWindow`, stop stretching to `WorkPos`/`WorkSize`. Give the
  launcher window a fixed size (~1450px wide × a height chosen to fit its layout,
  tunable — start ~880px) and position/flag it so ImGui promotes it to its own
  platform viewport; mark it `NoDocking` so it never docks into the editor.
- `EditorProjectManager` owns launcher visibility and the coexistence lifecycle:
  - **Startup:** editor main window opens (neutral backdrop, no project); launcher
    window shown + focused.
  - **Open project:** editor window raised/focused (`glfwFocusWindow`); the launcher
    **stays open** alongside it (the approved Unity-Hub coexistence). The user can
    close the launcher window manually; it is reopenable via a hub/menu action.
  - **Close/switch project:** launcher window shown + focused.
  - **Close editor main window:** app quits (unchanged). **Close launcher window:**
    hide it (reopenable), do not quit.
- Sizes are logical units; each viewport scales to its own monitor DPI via the
  existing `ConfigDpiScaleViewports`.
- **Verification:** launcher opens as a distinct ~1450px OS window; editor is a
  separate window; the two transition without any resize jank.

## Files touched (anticipated)

- `src/app/layers/DebugLayer.cpp` / `.hpp` — remove window-sizing policy (Phase 2).
- `src/app/Application.cpp` — simplify boot-window resolution (Phase 2).
- `src/app/debug/EditorProjectManager.cpp` / `.hpp` — launcher visibility + lifecycle
  (Phase 3); `kLauncherWindow*` repurposed to the fixed launcher size.
- `src/app/debug/ProjectLauncherWindow.cpp` — launcher as a fixed-size viewport window
  (Phase 3).
- `src/app/imgui/ImguiSubsystem.cpp` / `.hpp` (+ platform input glue) — viewport input
  routing (Phase 1).

## Testing

- Primarily UI-driven and verified per phase by the user, since the multi-viewport
  render path cannot be exercised headlessly here: (1) console scrolls in a floated
  window; (2) no editor resize when toggling the launcher; (3) launcher is its own
  1450px window coexisting with the editor.
- Any logic extracted from the deleted sizing policy (e.g. editor size persistence)
  gets doctest unit coverage.

## Risks

- The multi-viewport subsystem is complex, threaded (render-packet extraction), and
  runtime-unverified. **Phase 1 (input routing) is the deepest unknown** and gates the
  rest.
- Cannot be built/verified in this environment; each phase must be built and driven by
  the user before proceeding to the next.
- Secondary-viewport lifecycle (create/destroy on show/hide) must respect the existing
  quiesce-before-destroy handling (`SecondaryViewportIdsWithPendingDestroy`) to avoid
  tearing down a swapchain mid-render.
