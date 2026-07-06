# Editor UX Polish (Bucket A) - Design

Date: 2026-07-06
Status: Approved (design). Ready for implementation planning. Lands on the
`claude/imgui-multiviewport` branch. Pure Dear ImGui / editor-logic work — no render
passes, so it builds and unit-checks without a GPU; the "feel" is verified visually when
back at the machine.

## Goal

Take the AetherCore editor from functional to AAA-feel with five self-contained Dear ImGui
polish features: a polished inspector, a command palette, toasts + a console, dockspace
layout presets, and a play-mode toolbar. Companion spec: `2026-07-06-viewport-polish-design.md`
(Bucket B, render work).

## Decisions (locked with the user)

- Do all five Bucket A features; render-heavy viewport polish is a separate spec (Bucket B).
- Pure-ImGui / editor-logic only here — low risk, no GPU verification blocking the build.
- Sensible defaults chosen now (user is away); the few real choices are flagged inline.

## Background (verified)

- **Inspector:** `InspectorPanel::OnImGui` edits `SceneSelection::Primary()`, already shows
  an "Editing primary of N selected" hint, and calls per-component free functions
  (`DrawTransform`, `DrawMaterial`, `DrawLights`, `DrawBehaviors`, ...) in
  `ComponentDrawers`. The add-component popup already has a substring **filter** pattern.
  Uses `ICON_FA_*` glyphs.
- **Logging:** `Logger::Log(level, category, message, location)` is the single dispatch
  point (levels Verbose/Info/Warn/Error, categories); currently file + stdout. No in-memory
  sink yet.
- **Shell:** `DebugLayer` owns the dockspace (`DockBuilder`), the in-host menu bar, and the
  status bar; panels self-Begin with a `VisiblePtr()` open/closed bool. `PlayState` is a
  registered service. Chrome is gated to frame >= 1.

## Design

### 1. Inspector polish
- **New `InspectorWidgets.hpp/.cpp`** (app/debug) — shared field helpers the drawers route
  through:
  - `bool DragVec3(label, glm::vec3& v, float speed, glm::vec3 resetValue, const char* fmt)`
    — three drag-floats with X/Y/Z tinted red/green/blue (Unreal/Unity convention), a small
    reset-to-`resetValue` dot per row, and a hover tooltip. Returns edited.
  - Thin `DragFloatReset` / `ColorEditReset` wrappers with the same reset-dot affordance.
- `DrawTransform` uses `DragVec3` for position (reset 0), rotation (reset 0), scale (reset 1);
  `DrawLights`/`DrawBehaviors` adopt the color/scalar reset wrappers where natural.
- **Multi-select bulk edit (bounded):** when `selection.All().size() > 1`, after a property
  edit on the primary, propagate that property's new value to the rest of the selection
  (set-same-value) for scalar/color/vector props. Transform delta-propagation is **deferred**
  (per-entity absolute vs delta semantics are their own design). A helper
  `ApplyToSelection<T>(selection, world, memberPtr, value)` keeps this in one place.
- **Deferred (YAGNI):** an inspector-wide component search box (the sections are few; the
  add-component palette already has search).

### 2. Command palette (Ctrl+P)
- **New `EditorActions` registry** (app/debug): `struct EditorAction { std::string id, label;
  const char* icon; std::string shortcut; std::function<void(LayerContext&)> run; };` populated
  at `DebugLayer` init with: panel toggles (drive each panel's `VisiblePtr`), scene ops
  (new / save / delete selected / focus), play controls, and layout presets. Entity
  jump-to entries are rebuilt from the `World` when the palette opens.
- **New `CommandPalette`** (app/debug): `Ctrl+P` opens a centered popup with a search box +
  ranked results (subsequence fuzzy score, reusing the existing filter idea, ranked); Up/Down
  navigate, Enter runs the top hit, Esc closes. Drawn from `DebugLayer` (which has the key
  chord + service access).

### 3. Toasts + Console
- **New `LogRingBuffer`** (engine/utils, beside `Logger`): fixed-capacity, mutex-guarded ring
  of recent entries `{level, category, message, frameOrTime}`. Fed by a hook added to
  `Logger::Log` (guarded so it is a no-op until a buffer is installed — logging happens on
  multiple threads, so the push is locked).
- **New `ToastManager`** (app/debug): transient stacked notifications `{level, text, ttl}`
  drawn as a corner overlay on the main viewport, auto-expiring with a fade. API
  `Toast(level, text)`; Warn/Error logs optionally auto-toast (config bool, default on).
- **New `ConsolePanel`** (app/debug): a dockable panel over `LogRingBuffer` with per-level
  toggles, a category filter, text search, autoscroll, clear, and per-level row color. Added
  to the panel list + default dock layout.

### 4. Layout presets
- **New `LayoutStore`** (app/debug): named layouts, each an ImGui ini blob
  (`SaveIniSettingsToMemory`). Built-in "Default" (the current `DockBuilder` recipe) plus
  user-saved presets, persisted to a `layouts.ini`-style file resolved like the settings
  files.
- **Window ▸ Layout menu** in `DebugLayer`: list presets (apply via
  `LoadIniSettingsFromMemory`), "Save current as…", "Reset to Default" (re-runs the
  DockBuilder recipe). Applying is gated to frame >= 1 (chrome rule) and, if it restructures
  viewports, routes through the existing multi-viewport structural-quiesce path.

### 5. Play-mode toolbar
- A centered Play / Pause / Step control set in `DebugLayer` (a slim bar under the menu, or
  centered in the menu bar) that reads/writes the `PlayState` service; the active mode is
  highlighted (accent). Keyboard: reuse/extend existing shortcuts. Small, self-contained.

## File-by-file change list

- Create: `src/app/debug/InspectorWidgets.hpp/.cpp`, `EditorActions.hpp/.cpp`,
  `CommandPalette.hpp/.cpp`, `ToastManager.hpp/.cpp`, `ConsolePanel.hpp/.cpp`.
- Create: `src/engine/utils/LogRingBuffer.hpp/.cpp`.
- Modify: `src/app/debug/ComponentDrawers.cpp` (route vectors/colors through the new helpers
  + bulk-edit), `InspectorPanel.cpp` (bulk-edit wiring).
- Modify: `src/engine/utils/Logger.cpp` (ring-buffer sink hook).
- Modify: `src/app/layers/DebugLayer.cpp/.hpp` (register actions, palette key-chord, Layout
  menu, play toolbar, Console panel in the dock layout, ToastManager overlay).
- Modify: `src/app/Application.cpp` (construct/register `LogRingBuffer`, `ToastManager`,
  `LayoutStore` services).
- New engine files auto-register (GLOB_RECURSE); new **app** files may need adding to the app
  target's source list — confirm during planning (app may also glob).

## Verification

- Build MSVC (`build-vs2022-msvc`, Debug); `EngineTests` green (unit-test `LogRingBuffer`
  push/evict/filter and the fuzzy-match ranking — both pure logic).
- Visual pass on the machine: colored/reset inspector fields; `Ctrl+P` palette runs actions;
  toasts appear + expire; Console filters/searches; layouts save/apply; play toolbar drives
  PlayState. Validation clean.

## Out of scope (this spec)

- Transform delta bulk-edit across a multi-selection.
- Inspector-wide component search box.
- Any render pass (selection outline, viewport icons) — Bucket B.
