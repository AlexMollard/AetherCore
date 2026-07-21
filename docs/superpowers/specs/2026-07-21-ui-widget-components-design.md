# UI Widget Components — Design

**Date:** 2026-07-21
**Status:** Approved, ready for planning

## Goal

Make **Slider**, **Toggle**, **Button**, and **Progress Bar** first-class UI
components in the engine, so a widget is one component on one entity — authored
via MCP, serialized through reflection, driven by the engine — instead of a
hand-assembled cluster of entities re-positioned every frame by a per-screen
script.

## Motivation

The INKBOUND menu build exposed the hole. Today:

- A **slider** is three entities (`Track`/`Fill`/`Orb`) plus a bespoke
  `PositionSlider` method copied into each screen's script that reads
  `Ui.GetRect(track)` and writes `Ui.SetRect(fill/orb)` in absolute pixels every
  frame, plus a focus-pulse `Sin(_t)` in the same script.
- A **toggle** is a `Socket`/`Orb`/`Value` triple driven the same way.
- A **button** (every Title / Level Select menu item) is a bare `UISelectable` +
  `UIText` restyled by a per-frame loop in the screen script.

We already solved *focus* (the `UISelectable` + `UiNavigationSystem` selection
system). This solves *widgets*: the value, the sub-part geometry, and the
interaction move into the engine, and screen scripts shrink to "read value →
react."

This is the same "one component, one place" model the reflection-driven
component system is built around; widgets should not be an exception.

## Non-Goals / Scope Guardrails

- **Horizontal slider only.** Vertical sliders are YAGNI; add later if needed.
- **No theming system.** Colors are per-component fields. A global UI theme is a
  separate future effort.
- **Level Select migration deferred.** Its nodes are bespoke (background +
  number entity + detail-panel mirror), not plain buttons. They keep working
  unchanged this pass.
- **Progress Bar ships as a primitive**, not wired into a screen — its real
  consumers (ink-capacity meters, health, load bars) come later. It gets the
  component + editor-panel render + an MCP round-trip test only.

## Architecture Overview

Four new components in `src/engine/ui/UiComponents.hpp`, reflected in
`src/app/scene/reflection/MoreComponents.reflect.cpp` (one `AE_COMPONENT` block
each — auto-generates MCP add/set, catalog entry, inspector drawer, serializer).

Rendering extends `BuildDrawCommands` (`src/engine/ui/UiDrawBuilder.cpp`) to emit
each widget's sub-parts from the **existing** draw primitives (`kShapeRect`,
`kShapeCircle`, `kShapeSdfGlyph`) — no new shapes, no shader changes.

Interaction lives in a new `UiWidgetSystem` (`src/engine/ui/`) that runs in
`ScriptComponentSystem::Update` in the order:

```
UiNavigationSystem::Update  →  UiWidgetSystem::Update  →  scripts
```

so a script sees this frame's focus, this frame's committed value, and this
frame's `changed` flag.

Focus is by **composition**: Slider/Toggle/Button auto-compose a `UISelectable`,
so `UiNavigationSystem` is essentially unchanged (one small arbitration tweak,
below). Progress Bar has no `UISelectable` (not focusable).

## Components

All colors are `glm::vec4` RGBA. Runtime fields are never serialized (mirrors
`UISelectable::focused/activated` and `UIRect::resolvedRect`).

### UISlider

**Authored:**
- `float minValue = 0.f`
- `float maxValue = 1.f`
- `float step = 0.05f`   (0 = continuous)
- `float value = 0.5f`   (real units, clamped to [min,max])
- `glm::vec4 trackColor`
- `glm::vec4 fillColor`
- `glm::vec4 handleColor`
- `float handleRadius = 10.f`
- `float cornerRadius = 4.f`   (track rounding)

**Runtime:**
- `bool changed = false`   (true the one frame user input moved `value`)
- `bool dragging = false`

Normalized position `t = (value - minValue) / (maxValue - minValue)`, clamped
[0,1]. Fill width = inner-track-width × t. Handle centre x = inner-track-left +
fill width; handle centre y = track centre y. **Engine-auto focus feedback:** the
handle alpha pulses when the composed `UISelectable` is focused (this replaces
the per-script `Sin(_t)` pulse).

### UIToggle

**Authored:**
- `bool on = false`
- `glm::vec4 trackColor`   (track when off)
- `glm::vec4 onColor`      (track when on)
- `glm::vec4 knobColor`
- `float knobRadius = 9.f`
- `float cornerRadius = 12.f`   (pill track)

**Runtime:**
- `bool changed = false`

Pill track + knob circle. Knob x lerps left→right by `on`; track color lerps
`trackColor`→`onColor` by `on`. No text — the knob position *is* the state. (If a
screen wants a word like "FULL/OFF", it sets a separate script-driven `UIText`;
not part of the toggle.) Engine-auto focus feedback: knob brightens when focused.

### UIButton

**Authored:**
- `std::string label`
- `std::string fontName = "Roboto"`
- `float fontSize = 24.f`
- `UIText::HAlign hAlign = Center`
- `UIText::VAlign vAlign = Middle`
- `glm::vec4 bgColor`
- `glm::vec4 textColor`
- `glm::vec4 bgColorFocused`
- `glm::vec4 textColorFocused`
- `float cornerRadius = 4.f`

**Runtime:** none new — activation is read via the composed `UISelectable`
(`Ui.WasActivated`).

Background rounded rect + centered label glyphs. Focused (composed selectable) →
uses `bgColorFocused`/`textColorFocused`. Explicit focused colors (vs.
engine-auto) because menu-item styling is the most design-sensitive surface:
"text turns cyan" = set `textColorFocused`, keep `bgColorFocused == bgColor`;
"background fills" = set `bgColorFocused`. Covers both without a theme system.

### UIProgressBar

**Authored:**
- `float value = 0.f`   (normalized 0–1, clamped)
- `glm::vec4 trackColor`
- `glm::vec4 fillColor`
- `float cornerRadius = 4.f`

**Runtime:** none. No `UISelectable`, no interaction; value set by script or
reflection only.

## Focus & Interaction

### Composition

Adding a `UISlider`, `UIToggle`, or `UIButton` ensures the entity also has a
`UIRect` and a `UISelectable` (the entity factory adds them; the reflection/MCP
`add_component` path adds any missing companion). `UiNavigationSystem` continues
to iterate `View<UISelectable, UIRect>` — it does not know about widget types.

### Navigation arbitration (the one nav change)

A focused **horizontal slider captures `←`/`→`**: in `UiNavigationSystem`, when
the currently focused entity has a `UISlider`, skip *horizontal* spatial
movement (up/down still navigate). This lets the widget system own `←`/`→` for
value adjustment and removes today's fragile reliance on "sliders are stacked
vertically so ←/→ finds no neighbor." This is the only change to the nav system.

### UiWidgetSystem::Update(World&, Input&)

Runs after nav, before scripts. Per component:

- **UISlider** — clear `changed` at entry. If the composed selectable is focused
  and `Key::Right`/`Key::Left` pressed: `value = Quantize(value ± step)`, clamp,
  set `changed`. If hovered and `MouseButton::Left` is down: begin/continue
  `dragging`, `value = ValueFromMouseX(mouse.x, trackRect, min, max, step)`, set
  `changed` if it moved. Release ends `dragging`.
- **UIToggle** — clear `changed`. If the composed selectable's `activated` is set
  this frame (nav already sets it on Enter/Space/click): `on = !on`, set
  `changed`.
- **UIButton** — no work; scripts read `Ui.WasActivated`.
- **UIProgressBar** — no work.

Pure helpers (unit-tested, no ECS/Input deps):
- `float Normalized(float value, float min, float max)` → clamped [0,1]
- `float Quantize(float value, float min, float max, float step)` → nearest step
- `float ValueFromMouseX(float mouseX, glm::vec4 trackRect, float min, float max, float step)`

## Rendering

Extend `BuildDrawCommands` (`UiDrawBuilder.cpp`). Refactor UIText's glyph
emission into a shared internal helper `EmitText(out, rect, text, font, size,
color, hAlign, vAlign, layer)` so both `UIText` and `UIButton` use it.

Per widget, using `UIRect::resolvedRect` as the bounds, parts share the entity's
layer ordered back-to-front:

- **UISlider**: track rounded rect → fill rounded rect (inset) → handle circle.
- **UIToggle**: pill rect (color by `on`) → knob circle (position by `on`).
- **UIProgressBar**: track rect → fill rect.
- **UIButton**: background rounded rect → `EmitText` label.

The **UI Canvas editor panel** (`src/app/debug/UiCanvasPanel.cpp`,
`DrawPreviewElement`) gains matching preview rendering so authored widgets appear
as real widgets in the panel, not bounding boxes.

## C# API

`managed/AetherCore/Ui.cs` + `src/app/scripting/interop/UiExports.cpp` (new
`AE_SCRIPT_API` exports, each `TryGet<Component>` on `ActiveWorld()`):

```csharp
float  Ui.GetSliderValue(Entity e);
void   Ui.SetSliderValue(Entity e, float value);
bool   Ui.GetToggle(Entity e);
void   Ui.SetToggle(Entity e, bool on);
float  Ui.GetProgress(Entity e);
void   Ui.SetProgress(Entity e, float value);
string Ui.GetButtonLabel(Entity e);
void   Ui.SetButtonLabel(Entity e, string label);
bool   Ui.WasChanged(Entity e);   // true if a UISlider or UIToggle on e changed this frame
```

`Ui.IsFocused` / `Ui.WasActivated` / `Ui.SetFocus` / `Ui.SetInteractable` keep
working unchanged via the composed `UISelectable`.

## Reflection / MCP / Catalog / Icons

One `AE_COMPONENT` block per widget in `MoreComponents.reflect.cpp`. Runtime
fields marked so they are not serialized. Proposed FontAwesome icons (finalize
against `icons.hpp` in the plan):

- UI Slider → `ICON_FA_SLIDERS`
- UI Toggle → `ICON_FA_TOGGLE_ON`
- UI Button → `ICON_FA_SQUARE` (or a better fit found in `icons.hpp`)
- UI Progress Bar → `ICON_FA_BARS_PROGRESS`

Entity factories in `src/engine/ui/UiEntities.{hpp,cpp}`: `CreateSliderEntity`,
`CreateToggleEntity`, `CreateButtonEntity`, `CreateProgressBarEntity` — each adds
the widget component + `UIRect` (+ `UISelectable` for the interactive three).

## Migration (proof of the components)

1. **Settings screen** rebuilt on the widgets:
   - 3× `UISlider` (music / sfx / ink glow) replace the `SliderXTrack/Fill/Orb`
     triples.
   - 1× `UIToggle` replaces `ShakeSocket`/`ShakeOrb` (and the `ShakeValue`
     "FULL/OFF" text is removed).
   - `SettingsScreen.cs` collapses to: on `Ui.WasChanged(slider)` set the
     matching `GameSettings.*` from `Ui.GetSliderValue` + `Save`; on
     `Ui.WasChanged(toggle)` set `GameSettings.ScreenShake` from `Ui.GetToggle` +
     `Save`; back-link unchanged. **`PositionSlider` deleted.**
2. **Title screen** menu items (`descend` / `attune` / `release`) → `UIButton`
   with `textColorFocused` = accent cyan; TitleScreen's per-frame styling loop
   deleted.
3. **Level Select** unchanged (deferred).

## Testing

- **EngineTests**: unit tests for `Normalized`, `Quantize`, `ValueFromMouseX`
  (pure functions; TDD — write failing tests first).
- **Live gauntlet** (per prior UI work): build Editor → stop/rebuild/relaunch on
  `AETHER_CONTROL_PORT=8787` → MCP `add_component` each widget onto a scratch
  canvas, `set_component` fields, screenshot the UI Canvas panel → `play`,
  `send_input`/`play_input_sequence` to drive slider (←/→ + drag) and toggle
  (Enter) → screenshot viewport, verify value moves and persists → verify the
  migrated Settings + Title screens still behave (sliders adjust, toggle flips,
  buttons focus/activate, back-link returns).

## Risks / Notes

- **Stale managed cache trap:** SDK edits (`Ui.cs`) require a `ManagedAssemblies`
  (or Editor) rebuild *and* clearing `projects/INKBOUND/Builds/Intermediate/
  managed` + the project `scripts/obj|bin`, or Play loads a stale `AetherGame.dll`
  ("Unknown C# script type"). See memory `aethercore-inkbound-ui-build`.
- **Companion-component add:** confirm the MCP/catalog `add_component` path can
  add `UISelectable`/`UIRect` alongside a widget (or that the entity factory is
  the authoring route). If MCP can't add companions, author widgets via the
  factory-backed create path and document it.
- **ASCII-only UI text** still applies to Button labels (no multibyte glyphs).
- **Engine build → editor restart** required for each C++ change; run from the
  build-tree root so `shaders://` resolves.
