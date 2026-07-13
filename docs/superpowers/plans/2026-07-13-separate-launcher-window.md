# Separate Launcher Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the project launcher its own fixed ~1450px OS window that coexists with the editor window, and delete the per-frame force-resize policy causing the 1080↔1440 oscillation.

**Architecture:** Reuse the existing ImGui multi-viewport path (`ViewportsEnable` + custom threaded per-viewport Vulkan rendering). The editor is the ImGui *main viewport*; the launcher is a *secondary viewport* OS window. Work is phased: (1) verify secondary-viewport input works (harden only if a real defect surfaces), (2) delete the force-resize policy, (3) make the launcher its own fixed-size viewport window + coexistence lifecycle.

**Note:** the console scroll bug once thought to share a root here was actually a separate ConsolePanel `ImGuiListClipper` + multi-line-entry defect (see the standalone ConsolePanel fix), so Phase 1 is now a *verification* step, not an assumed fix.

**Tech Stack:** C++23, Dear ImGui v1.92 (docking + viewports), GLFW 3.4, Vulkan, doctest, CMake (MSVC `build/vs2022-msvc` is the working build).

**Spec:** `docs/superpowers/specs/2026-07-13-separate-launcher-window-design.md`

---

## Verification model (read first)

This is a windowing/rendering feature. **It cannot be verified headlessly** — every
phase ends with a manual "drive the app" check. Reference commands:

- **Build App (MSVC/Debug):** `cmake --build build/vs2022-msvc --config Debug --target App`
- **Run:** launch from the build-tree root so `shaders://` resolves:
  `cd build/vs2022-msvc && ./src/app/Debug/App.exe`
- **Build+run unit tests (only where a task adds them):**
  `cmake --build build/vs2022-msvc --config Debug --target EngineTests && ./build/vs2022-msvc/tests/Debug/EngineTests.exe --test-case="<name>"`

Do the phases **in order** and get a green manual check on each before starting the
next. Phase 1 gates the rest.

---

## File structure

- `src/app/imgui/ImguiSubsystem.cpp` / `.hpp` — multi-viewport input routing (Phase 1).
- `src/app/layers/DebugLayer.cpp` / `.hpp` — remove the window-sizing policy (Phase 2).
- `src/app/Application.cpp` — simplify boot-window resolution (Phase 2).
- `src/app/debug/EditorWindowState.hpp` / `.cpp` **(new)** — small, unit-testable helper
  that decides the editor boot window size + persists resize changes (Phase 2).
- `src/app/debug/EditorProjectManager.cpp` / `.hpp` — launcher visibility + coexistence
  lifecycle; repurpose `kLauncherWindow*` to the fixed launcher size (Phase 3).
- `src/app/debug/ProjectLauncherWindow.cpp` — launcher as a fixed-size viewport window
  (Phase 3).
- `tests/app/EditorWindowStateTests.cpp` **(new)** — doctest for the size helper (Phase 2).

---

## Phase 1 — Verify secondary-viewport input (harden only if broken)

**Why first:** the launcher must be an interactive secondary-viewport window, so
confirm input already routes to floated viewport windows before building on it.
**Verify first:** float a panel out (e.g. Hierarchy) and test wheel / scroll / click /
text-field keyboard / context menu. If it all works — likely, since the console scroll
bug that seemed to implicate viewport input was actually a separate ConsolePanel
`ImGuiListClipper` + multi-line-entry defect — record "secondary-viewport input
verified" and skip straight to Phase 2. Only if a genuine input defect surfaces do
Tasks 1.1–1.2 below apply (they diagnose + fix the platform input wiring).

### Task 1.1: Reproduce and root-cause secondary-viewport input

**Files:**
- Read: `src/app/imgui/ImguiSubsystem.cpp` (esp. init `ImGui_ImplGlfw_InitForVulkan` at
  ~597, and the "suppress the stock Vulkan renderer viewport hooks" note at ~636),
  `src/app/imgui/ImguiSubsystem.hpp`, and the platform-input wiring.

- [ ] **Step 1: Reproduce.** Build + run App, dock the Console panel, then drag it out
  of the main window so it becomes its own OS window. Try mouse-wheel scroll and
  scrollbar drag inside it.
  Expected today: scrolling and/or clicks do not register in the floated window.

- [ ] **Step 2: Confirm it is input routing, not the panel.** Dock the console back
  inside the main window; confirm it scrolls fine there. This isolates the defect to
  the secondary-viewport (floated) case.

- [ ] **Step 3: Instrument.** In the floated state, log `ImGui::GetIO().MousePos`,
  `io.MouseWheel`, and `ImGui::FindViewportByPlatformHandle(...)` (or the hovered
  viewport id) for a few frames while hovering the floated window. Determine whether
  ImGui receives the events at all, and whether they carry the correct viewport.

- [ ] **Step 4: Identify the cause.** Compare against a stock ImGui multi-viewport
  setup. The strong candidates, given `ImguiSubsystem.cpp:636` suppresses the stock
  Vulkan viewport hooks: (a) GLFW input callbacks are not installed on
  platform-created viewport windows (so wheel/keyboard never reach ImGui for them);
  (b) `ImGui_ImplGlfw`'s per-viewport callback ownership was disturbed when the
  renderer hooks were suppressed; (c) `io.ConfigViewportsNoDecoration`/focus handling
  drops input. Write the identified cause into the task's commit message so Task 1.2
  is grounded.

- [ ] **Step 5: Commit the findings** (a short `docs/` note or an inline comment in
  `ImguiSubsystem.cpp` documenting the root cause — no behavior change yet).

```bash
git add -A && git commit -m "Diagnose secondary-viewport input routing (console scroll)"
```

### Task 1.2: Fix secondary-viewport input routing

**Files:**
- Modify: `src/app/imgui/ImguiSubsystem.cpp` (and `.hpp` if a hook needs exposing).

- [ ] **Step 1: Apply the fix from Task 1.1.** Implement the routing fix the diagnosis
  pointed to. The most likely concrete form, if cause (a)/(b): ensure
  `ImGui_ImplGlfw` installs its input callbacks on each platform viewport window —
  i.e. do **not** suppress the *platform* (GLFW) callbacks when suppressing the stock
  *renderer* (Vulkan) viewport hooks; only the renderer `Renderer_*` platform-IO
  hooks should be overridden, leaving `Platform_*` input callbacks intact. Verify the
  suppression at `ImguiSubsystem.cpp:636` only clears the renderer hooks and re-install
  the GLFW callbacks for existing viewport windows if they were cleared.

- [ ] **Step 2: Build.** `cmake --build build/vs2022-msvc --config Debug --target App`
  Expected: builds clean.

- [ ] **Step 3: Manual verify (the acceptance test for Phase 1).** Run App, float the
  Console panel into its own window, and confirm: mouse-wheel scrolls it, the
  scrollbar drags, row clicks/selection and the right-click context menu all work.
  Float a second panel (e.g. Hierarchy) and confirm it too.

- [ ] **Step 4: Regression check.** Confirm docked panels still scroll/click, and the
  main editor window is unaffected.

- [ ] **Step 5: Commit.**

```bash
git add -A && git commit -m "Route input to secondary ImGui viewport windows (fixes console scroll)"
```

---

## Phase 2 — Delete the force-resize policy

**Goal:** the editor window owns its own size; nothing force-`SetSize`s it. This alone
removes the 1080↔1440 oscillation.

### Task 2.1: Extract editor window-size logic into a unit-testable helper

**Files:**
- Create: `src/app/debug/EditorWindowState.hpp`, `src/app/debug/EditorWindowState.cpp`
- Create: `tests/app/EditorWindowStateTests.cpp`
- Modify: `tests/CMakeLists.txt` (add the new test source)

- [ ] **Step 1: Write the failing test.**

```cpp
// tests/app/EditorWindowStateTests.cpp
#include <doctest/doctest.h>
#include "debug/EditorWindowState.hpp"

using aether::app::ResolveEditorWindowSize;

TEST_CASE("ResolveEditorWindowSize prefers a valid saved size")
{
    // saved 2560x1440, defaults 1920x1080 -> uses saved
    const auto s = ResolveEditorWindowSize(2560, 1440, 1920, 1080);
    CHECK(s.width == 2560);
    CHECK(s.height == 1440);
}

TEST_CASE("ResolveEditorWindowSize falls back to defaults for degenerate saved sizes")
{
    const auto s = ResolveEditorWindowSize(0, 0, 1920, 1080);
    CHECK(s.width == 1920);
    CHECK(s.height == 1080);
    const auto tiny = ResolveEditorWindowSize(50, 40, 1920, 1080);
    CHECK(tiny.width == 1920);
    CHECK(tiny.height == 1080);
}
```

- [ ] **Step 2: Run to verify it fails.**
  `cmake --build build/vs2022-msvc --config Debug --target EngineTests`
  Expected: FAIL to compile (`EditorWindowState.hpp` not found).

- [ ] **Step 3: Implement the helper.**

```cpp
// src/app/debug/EditorWindowState.hpp
#pragma once
namespace aether::app
{
    struct EditorWindowSize { int width; int height; };

    // Saved editor size if valid (>= 640x480), else the defaults. No launcher
    // size involved - the launcher never drives the editor window anymore.
    [[nodiscard]] EditorWindowSize ResolveEditorWindowSize(int savedW, int savedH, int defaultW, int defaultH);
}
```

```cpp
// src/app/debug/EditorWindowState.cpp
#include "debug/EditorWindowState.hpp"
namespace aether::app
{
    EditorWindowSize ResolveEditorWindowSize(int savedW, int savedH, int defaultW, int defaultH)
    {
        if (savedW >= 640 && savedH >= 480)
        {
            return {savedW, savedH};
        }
        return {defaultW, defaultH};
    }
}
```

- [ ] **Step 4: Run to verify it passes.**
  `cmake --build build/vs2022-msvc --config Debug --target EngineTests && ./build/vs2022-msvc/tests/Debug/EngineTests.exe --test-case="ResolveEditorWindowSize*"`
  Expected: PASS (2 cases).

- [ ] **Step 5: Commit.**

```bash
git add src/app/debug/EditorWindowState.* tests/app/EditorWindowStateTests.cpp tests/CMakeLists.txt
git commit -m "Add ResolveEditorWindowSize helper (editor-owned window size)"
```

### Task 2.2: Simplify boot-window resolution to always use the editor size

**Files:**
- Modify: `src/app/Application.cpp` (`ResolveEditorBootWindow`, ~48-59)

- [ ] **Step 1:** Replace the launcher-vs-editor branch so the editor window always
  opens at the resolved editor size. Read the saved `editor.window_width/height` from
  `EditorState.toml` and pass through `ResolveEditorWindowSize(savedW, savedH,
  settings.window.width, settings.window.height)`. Remove the `reopensProject`
  branch and the `kLauncherWindowWidth/Height` return.

- [ ] **Step 2: Build.** `cmake --build build/vs2022-msvc --config Debug --target App`
  Expected: builds clean.

- [ ] **Step 3: Commit.**

```bash
git add src/app/Application.cpp && git commit -m "Boot the editor window at the editor size, not the launcher size"
```

### Task 2.3: Remove the per-frame window-sizing policy from DebugLayer

**Files:**
- Modify: `src/app/layers/DebugLayer.cpp` (delete `UpdateWindowSizing` ~811-853,
  `CaptureEditorWindowSize` ~798-809, and the `UpdateWindowSizing(context)` call in
  `OnImGui` ~924)
- Modify: `src/app/layers/DebugLayer.hpp` (delete `m_editorWindowW/H`,
  `m_windowSettleFrames`, `m_launcherModeTracked`, `m_windowModeInit`, and the two
  method declarations)

- [ ] **Step 1: Delete** the two method bodies and their `OnImGui` call site.

- [ ] **Step 2: Add** minimal size persistence in their place: once per frame (or on a
  GLFW resize callback), if `!m_projects.IsLauncherOpen()`, write the current
  `Window::GetWindowSize()` to `EditorState.toml` `editor.window_width/height` (debounced
  — only when it changed). This preserves "editor remembers its size" without the
  force/restore dance.

- [ ] **Step 3: Delete** the member variables from the header and any remaining
  references.

- [ ] **Step 4: Build.** `cmake --build build/vs2022-msvc --config Debug --target App`
  Expected: builds clean (no unresolved refs to the removed members).

- [ ] **Step 5: Manual verify (Phase 2 acceptance).** Run App. Confirm: opening the
  launcher and picking a project no longer resizes the OS window; the 1080↔1440
  oscillation is gone; manually resize the editor window, restart, and confirm the
  size is remembered.

- [ ] **Step 6: Commit.**

```bash
git add src/app/layers/DebugLayer.* && git commit -m "Delete the per-frame force-resize policy; editor window owns its size"
```

---

## Phase 3 — Launcher as its own fixed-size viewport window

**Goal:** the launcher is a distinct ~1450px OS window coexisting with the editor.

### Task 3.1: Turn the launcher into a fixed-size, own-viewport window

**Files:**
- Modify: `src/app/debug/ProjectLauncherWindow.cpp` (~196-206, the fullscreen setup)
- Modify: `src/app/debug/EditorProjectManager.hpp` (repurpose `kLauncherWindowWidth`
  to `1450`; set a fixed `kLauncherWindowHeight`, start `880`)

- [ ] **Step 1:** Replace `SetNextWindowPos(viewport->WorkPos)` /
  `SetNextWindowSize(viewport->WorkSize)` with a fixed size
  `ImGui::SetNextWindowSize(ImVec2(kLauncherWindowWidth, kLauncherWindowHeight))` and a
  position that lands it **outside** the main viewport's rect on first show, so ImGui
  promotes it to its own platform viewport (e.g. centered on the primary monitor work
  area via `ImGui::GetPlatformIO().Monitors[0]`). Add `ImGuiWindowFlags_NoDocking` and
  drop `NoResize`-fullscreen assumptions; keep the launcher's existing background/content
  drawing but sized to the fixed window.

- [ ] **Step 2:** Confirm the launcher `Begin` uses a stable id and is only submitted
  while the launcher should be visible (guard on `m_projects.IsLauncherOpen()`).

- [ ] **Step 3: Build + manual verify.** Run App. The launcher appears as its **own OS
  window** at 1450×880, separate from the editor window, and its content lays out at
  the intended width. Input works (relies on Phase 1).

- [ ] **Step 4: Commit.**

```bash
git add src/app/debug/ProjectLauncherWindow.cpp src/app/debug/EditorProjectManager.hpp
git commit -m "Launcher is its own fixed 1450px viewport window"
```

### Task 3.2: Coexistence lifecycle

**Files:**
- Modify: `src/app/debug/EditorProjectManager.cpp` (`OpenProject`, `OpenLauncher`,
  `CloseLauncher`, and window focus)
- Read: `src/engine/platform/Window.hpp` for a focus/raise call; add
  `Window::Focus()` (wrapping `glfwFocusWindow`) if none exists.

- [ ] **Step 1: Startup.** Editor main window opens (neutral/empty backdrop, no
  project); launcher window shown + focused. Verify `LoadSettings`/boot path shows the
  launcher by default when no project auto-opens.

- [ ] **Step 2: Open project.** In `OpenProject`, after the project loads, raise/focus
  the **editor** main window; leave the launcher window open (Unity-Hub coexistence).

- [ ] **Step 3: Close/switch project.** Show + focus the **launcher** window.

- [ ] **Step 4: Window close semantics.** Closing the editor main window quits
  (unchanged). Closing the launcher window hides it (do not quit); it is reopenable via
  the existing hub/menu action (`OpenLauncher`). Wire the launcher viewport's close
  request to `CloseLauncher` rather than app exit.

- [ ] **Step 5: Build + manual verify (Phase 3 acceptance).** Run App: launcher is its
  own window; pick a project → editor window focuses, launcher stays open; switch/close
  project → launcher focuses; close launcher → it hides, editor stays; close editor →
  app quits. No resize jank anywhere.

- [ ] **Step 6: Commit.**

```bash
git add -A && git commit -m "Launcher/editor coexistence lifecycle (raise, hide, close semantics)"
```

### Task 3.3: Secondary-viewport teardown safety

**Files:**
- Read/modify: `src/app/imgui/ImguiSubsystem.cpp`
  (`SecondaryViewportIdsWithPendingDestroy`, ~261) and the launcher hide path.

- [ ] **Step 1:** Confirm hiding the launcher (stopping its `Begin` submission) lets the
  existing `SecondaryViewportIdsWithPendingDestroy` / quiesce path tear its swapchain
  down safely — i.e. the launcher window destroy goes through the same
  quiesce-before-destroy as a floated panel, never mid-render.

- [ ] **Step 2: Manual verify.** Repeatedly open→close the launcher and open→switch
  projects; watch the log for Vulkan validation errors or a null-handle crash on
  viewport destroy. Expected: clean, no validation errors.

- [ ] **Step 3: Commit.**

```bash
git add -A && git commit -m "Verify launcher viewport teardown follows the quiesce path"
```

---

## Self-review notes

- **Spec coverage:** Phase 1 ↔ "input hardening / console fix"; Phase 2 ↔ "delete
  force-resize policy" + boot-size simplification + size persistence; Phase 3 ↔
  "launcher as own 1450px viewport" + "coexistence lifecycle" + the teardown-safety
  risk called out in the spec. Editor-as-main / launcher-as-secondary mapping is in
  3.1/3.2. DPI is inherited from `ConfigDpiScaleViewports` (no task needed).
- **Known soft spots (by design, not placeholders):** Task 1.2's exact fix is
  contingent on Task 1.1's diagnosis — the plan names the most likely concrete form but
  the live repro decides. Launcher height `880` and the "position outside main viewport
  to force promotion" mechanism are the two values/techniques most likely to need
  tuning during 3.1.
