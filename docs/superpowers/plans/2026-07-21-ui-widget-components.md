# UI Widget Components Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Slider, Toggle, Button, and Progress Bar first-class engine UI components — one component on one entity, engine-drawn and engine-driven — replacing the hand-assembled entity clusters + per-screen scripts used today.

**Architecture:** Four POD components in `src/engine/ui/UiComponents.hpp`, reflected in `MoreComponents.reflect.cpp` (auto MCP/catalog/inspector/serde). Rendering extends `BuildDrawCommands` using the existing rect/circle/glyph primitives — no shader changes. Interaction lives in a new stateless `UiWidgetSystem` that runs between `UiNavigationSystem` and scripts. Focus is by composition: interactive widgets auto-compose a `UISelectable` (via a `PostSet` hook fired on add), so the nav system is unchanged but for one arbitration rule. A thin C# value API is added to `Ui.cs`.

**Tech Stack:** C++20, entt ECS, glm, Slang UI shader (unchanged), C#/CoreCLR (`LibraryImport` interop), doctest (`EngineTests`), aethercore MCP for live authoring/verification.

## Global Constraints

- **Horizontal slider only.** No vertical orientation this pass.
- **No theming system.** Colors are per-component fields.
- **UI text is ASCII-only.** Button labels included — no multibyte glyphs.
- **Engine C++ change ⇒ editor restart.** Stop `Editor.exe`, rebuild, relaunch the Launcher on `AETHER_CONTROL_PORT=8787`. Run the editor from the build-tree root so `shaders://` resolves.
- **SDK (`Ui.cs`/`Native.cs`) change ⇒ clear the stale managed cache** before Play: rebuild `ManagedAssemblies` **and** `rm -rf projects/INKBOUND/Builds/Intermediate/managed projects/INKBOUND/scripts/obj projects/INKBOUND/scripts/bin`, or Play loads a stale `AetherGame.dll` ("Unknown C# script type"). Use `bash rm` (PowerShell `Remove-Item` is hook-blocked on `D:\AetherCore`).
- **Build dir:** `build/vs2022-msvc` (Debug). Tests: `ctest --test-dir build/vs2022-msvc -C Debug -R EngineTests --output-on-failure`.
- **Finish:** fast-forward merge + push to `master` (already the working branch).

## File Structure

**Engine (`src/engine/ui/`)**
- `UiComponents.hpp` — add `UISlider`, `UIToggle`, `UIButton`, `UIProgressBar`.
- `UiWidgetSystem.hpp` / `UiWidgetSystem.cpp` (NEW) — pure value helpers + `Update(World&, Input&, float time)`.
- `UiNavigationSystem.cpp` — slider-captures-left/right arbitration (one block).
- `UiDrawBuilder.cpp` — refactor `EmitText` → shared `EmitTextRun`; emit widget parts in `Walk`.
- `UiEntities.hpp` / `UiEntities.cpp` — `CreateSliderEntity` / `CreateToggleEntity` / `CreateButtonEntity` / `CreateProgressBarEntity`.

**App (`src/app/`)**
- `scene/reflection/MoreComponents.reflect.cpp` — four `AE_COMPONENT` blocks + `PostSet` auto-compose.
- `editor/ComponentCatalog.cpp` — fire `postSet` from the auto-generated `add` (one line).
- `scripting/interop/UiExports.cpp` — new exports.
- `debug/UiCanvasPanel.cpp` — widget preview rendering.

**Managed (`managed/AetherCore/`)**
- `Internal/Native.cs` — `LibraryImport` decls.
- `Ui.cs` — public API.

**Tests (`tests/ui/`)** (globbed — no CMake edit)
- `UiWidgetSystemTests.cpp` (NEW) — value-math helpers.
- `UiDrawBuilderTests.cpp` — widget emission.

**Game (`projects/INKBOUND/`)**
- `scenes/Menu.scene.toml`, `scripts/SettingsScreen.cs`, `scripts/TitleScreen.cs` — migration (authored via MCP; scene saved by `save_scene`).

---

## Phase 1 — Components, Reflection, Factories

### Task 1.1: Add the four widget component structs

**Files:**
- Modify: `src/engine/ui/UiComponents.hpp`

**Interfaces:**
- Produces: `aether::ui::UISlider`, `UIToggle`, `UIButton`, `UIProgressBar`. Runtime-only fields (`changed`/`dragging`/`pulse`) are never reflected/serialized (mirrors `UIRect::resolvedRect`, `UISelectable::focused`).

- [ ] **Step 1: Append the structs** to `UiComponents.hpp` inside `namespace aether::ui`, after `UIText`. Reuse `UIText::HAlign`/`VAlign` for the button.

```cpp
	// Interactive horizontal slider. One entity; the draw builder renders track+fill+handle
	// and UiWidgetSystem drives value from keyboard (when focused) and mouse drag. Composes a
	// UISelectable for focus (added by the reflection PostSet hook).
	struct UISlider
	{
		float minValue = 0.f;
		float maxValue = 1.f;
		float step = 0.05f; // 0 = continuous
		float value = 0.5f; // real units, clamped to [min,max]
		glm::vec4 trackColor{0.10f, 0.11f, 0.14f, 1.f};
		glm::vec4 fillColor{0.30f, 0.85f, 1.f, 1.f};
		glm::vec4 handleColor{0.30f, 0.85f, 1.f, 1.f};
		float handleRadius = 10.f;
		float cornerRadius = 4.f;
		bool changed = false;   // runtime: user moved value this frame
		bool dragging = false;  // runtime: mouse drag in progress
		float pulse = 1.f;      // runtime: focus alpha multiplier for the handle
	};

	struct UIToggle
	{
		bool on = false;
		glm::vec4 trackColor{0.15f, 0.17f, 0.22f, 1.f}; // track when off
		glm::vec4 onColor{0.30f, 0.85f, 1.f, 1.f};       // track when on
		glm::vec4 knobColor{0.90f, 0.95f, 1.f, 1.f};
		float knobRadius = 9.f;
		float cornerRadius = 12.f; // pill
		bool changed = false; // runtime
		float pulse = 1.f;    // runtime
	};

	struct UIButton
	{
		std::string label;
		std::string fontName = "Roboto";
		float pixelSize = 24.f;
		UIText::HAlign hAlign = UIText::HAlign::Center;
		UIText::VAlign vAlign = UIText::VAlign::Middle;
		glm::vec4 bgColor{0.07f, 0.08f, 0.10f, 1.f};
		glm::vec4 textColor{0.70f, 0.75f, 0.85f, 1.f};
		glm::vec4 bgColorFocused{0.07f, 0.08f, 0.10f, 1.f};
		glm::vec4 textColorFocused{0.30f, 0.85f, 1.f, 1.f};
		float cornerRadius = 4.f;
	};

	// Read-only meter. No interaction, no UISelectable.
	struct UIProgressBar
	{
		float value = 0.f; // normalized 0..1
		glm::vec4 trackColor{0.10f, 0.11f, 0.14f, 1.f};
		glm::vec4 fillColor{0.30f, 0.85f, 1.f, 1.f};
		float cornerRadius = 4.f;
	};
```

- [ ] **Step 2: Build the engine** to confirm the header compiles.

Run: `cmake --build build/vs2022-msvc --target Engine --config Debug`
Expected: builds clean.

- [ ] **Step 3: Commit.**

```bash
git add src/engine/ui/UiComponents.hpp
git commit -m "Add UISlider/UIToggle/UIButton/UIProgressBar components"
```

---

### Task 1.2: Reflect the widgets + auto-compose companions

Makes all four MCP-addable, serialized, inspector-editable. Interactive three auto-compose `UIRect` + `UISelectable` via a `PostSet` hook, and the catalog's auto-generated `add` is taught to fire `postSet` so a bare `add_component` composes them too.

**Files:**
- Modify: `src/app/scene/reflection/MoreComponents.reflect.cpp`
- Modify: `src/app/editor/ComponentCatalog.cpp:294`

**Interfaces:**
- Consumes: `aether::ui::UISlider/UIToggle/UIButton/UIProgressBar` (Task 1.1); `ComponentBuilder::PostSet` (`Reflection.hpp:413`).
- Produces: component names "UI Slider", "UI Toggle", "UI Button", "UI Progress Bar".

- [ ] **Step 1: Add type aliases** near the top of `MoreComponents.reflect.cpp` (after line 19, with the other UI aliases). `AE_COMPONENT` pastes an unqualified token.

```cpp
using UiSliderComponent = aether::ui::UISlider;
using UiToggleComponent = aether::ui::UIToggle;
using UiButtonComponent = aether::ui::UIButton;
using UiProgressBarComponent = aether::ui::UIProgressBar;
```

- [ ] **Step 2: Add a helper** in the anonymous namespace (after `UiVAlignEnum`, before its closing `}`), used by the interactive widgets' `PostSet` to compose companions idempotently:

```cpp
	void EnsureWidgetCompanions(World& world, Entity e, bool selectable)
	{
		if (!world.Has<aether::ui::UIRect>(e))
		{
			auto& r = world.Emplace<aether::ui::UIRect>(e);
			r.anchorMin = {0.5f, 0.5f};
			r.anchorMax = {0.5f, 0.5f};
			r.offsetMin = {-110.f, -14.f};
			r.offsetMax = {110.f, 14.f};
		}
		if (selectable && !world.Has<aether::ui::UISelectable>(e))
		{
			world.Emplace<aether::ui::UISelectable>(e);
		}
	}
```

Add `#include "scene/World.hpp"` and `#include "scene/Entity.hpp"` at the top if not already transitively available (verify against a clean build).

- [ ] **Step 3: Add the four `AE_COMPONENT` blocks** at the end of the file (after the `UiSelectableComponent` block, before the final EOF). Enums reuse `UiHAlignEnum()`/`UiVAlignEnum()`.

```cpp
AE_COMPONENT(UiSliderComponent, "UI Slider", "UI", ICON_FA_SLIDERS)
AE_FIELD_N("min", minValue, Float)
AE_FIELD_N("max", maxValue, Float)
AE_FIELD_N("step", step, Float)
AE_FIELD_N("value", value, Float)
AE_FIELD_N("track_color", trackColor, Color4)
AE_FIELD_N("fill_color", fillColor, Color4)
AE_FIELD_N("handle_color", handleColor, Color4)
AE_FIELD_N("handle_radius", handleRadius, Float)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiToggleComponent, "UI Toggle", "UI", ICON_FA_TOGGLE_ON)
AE_FIELD_N("on", on, Bool)
AE_FIELD_N("track_color", trackColor, Color4)
AE_FIELD_N("on_color", onColor, Color4)
AE_FIELD_N("knob_color", knobColor, Color4)
AE_FIELD_N("knob_radius", knobRadius, Float)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiButtonComponent, "UI Button", "UI", ICON_FA_SQUARE)
AE_FIELD_N("label", label, String)
AE_FIELD_N("font", fontName, String)
AE_FIELD_N("pixel_size", pixelSize, Float)
AE_FIELD_ENUM("h_align", hAlign, UiHAlignEnum())
AE_FIELD_ENUM("v_align", vAlign, UiVAlignEnum())
AE_FIELD_N("bg_color", bgColor, Color4)
AE_FIELD_N("text_color", textColor, Color4)
AE_FIELD_N("bg_color_focused", bgColorFocused, Color4)
AE_FIELD_N("text_color_focused", textColorFocused, Color4)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiProgressBarComponent, "UI Progress Bar", "UI", ICON_FA_BARS_PROGRESS)
AE_FIELD_N("value", value, Float)
AE_FIELD_N("track_color", trackColor, Color4)
AE_FIELD_N("fill_color", fillColor, Color4)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, false); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
```

- [ ] **Step 4: Fire `postSet` from the catalog's auto-generated `add`.** In `ComponentCatalog.cpp:294`, change the add lambda so composed companions appear on a bare `add_component` (currently `postSet` only fires on set/apply):

```cpp
// before:
[&rt](World& w, Entity e, ServiceContainer&) { rt.emplaceDefault(w, e); },
// after:
[&rt](World& w, Entity e, ServiceContainer&) { rt.emplaceDefault(w, e); if (rt.postSet) { rt.postSet(w, e); } },
```

- [ ] **Step 5: Build the editor.**

Run: `cmake --build build/vs2022-msvc --target Editor --config Debug`
Expected: builds clean.

- [ ] **Step 6: Restart the editor and confirm the types register.** Stop `Editor.exe`, relaunch on `AETHER_CONTROL_PORT=8787`, then via MCP `list_component_types` confirm "UI Slider", "UI Toggle", "UI Button", "UI Progress Bar" are present with the fields above.

- [ ] **Step 7: Commit.**

```bash
git add src/app/scene/reflection/MoreComponents.reflect.cpp src/app/editor/ComponentCatalog.cpp
git commit -m "Reflect UI widgets and auto-compose their companions"
```

---

### Task 1.3: Widget entity factories

Script/editor create path that emplaces the widget + a correctly-sized `UIRect` (+ `UISelectable` for interactive ones) under a canvas.

**Files:**
- Modify: `src/engine/ui/UiEntities.hpp`
- Modify: `src/engine/ui/UiEntities.cpp`

**Interfaces:**
- Produces: `Entity CreateSliderEntity(World&, Entity canvas)`, `CreateToggleEntity`, `CreateButtonEntity`, `CreateProgressBarEntity`.
- Consumes: existing `EnsureCanvas` (static, in the .cpp anonymous namespace).

- [ ] **Step 1: Declare** the four factories in `UiEntities.hpp` after `CreateTextEntity`:

```cpp
	Entity CreateSliderEntity(World& world, Entity canvas);
	Entity CreateToggleEntity(World& world, Entity canvas);
	Entity CreateButtonEntity(World& world, Entity canvas);
	Entity CreateProgressBarEntity(World& world, Entity canvas);
```

- [ ] **Step 2: Implement** them in `UiEntities.cpp` (add `#include "ui/UiComponents.hpp"` already present). Each follows the `CreateImageEntity` shape: create, name, hierarchy, sized `UIRect`, the widget component, `UISelectable` for interactive ones, parent to canvas.

```cpp
	Entity CreateSliderEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);
		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "Slider"});
		world.Emplace<HierarchyComponent>(e);
		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-110.f, -12.f};
		rect.offsetMax = {110.f, 12.f};
		world.Emplace<UISlider>(e);
		world.Emplace<UISelectable>(e);
		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateToggleEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);
		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "Toggle"});
		world.Emplace<HierarchyComponent>(e);
		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-26.f, -13.f};
		rect.offsetMax = {26.f, 13.f};
		world.Emplace<UIToggle>(e);
		world.Emplace<UISelectable>(e);
		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateButtonEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);
		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "Button"});
		world.Emplace<HierarchyComponent>(e);
		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-120.f, -24.f};
		rect.offsetMax = {120.f, 24.f};
		auto& btn = world.Emplace<UIButton>(e);
		btn.label = "Button";
		world.Emplace<UISelectable>(e);
		ecs::SetParent(world, e, canvas);
		return e;
	}

	Entity CreateProgressBarEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);
		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "ProgressBar"});
		world.Emplace<HierarchyComponent>(e);
		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-110.f, -8.f};
		rect.offsetMax = {110.f, 8.f};
		world.Emplace<UIProgressBar>(e);
		ecs::SetParent(world, e, canvas);
		return e;
	}
```

- [ ] **Step 3: Build the engine.**

Run: `cmake --build build/vs2022-msvc --target Engine --config Debug`
Expected: builds clean.

- [ ] **Step 4: Commit.**

```bash
git add src/engine/ui/UiEntities.hpp src/engine/ui/UiEntities.cpp
git commit -m "Add widget entity factories"
```

---

## Phase 2 — Rendering

### Task 2.1: Extract `EmitTextRun` (no behavior change)

Refactor so Button can reuse glyph emission. Existing `UIText` behavior and its tests stay green.

**Files:**
- Modify: `src/engine/ui/UiDrawBuilder.cpp`

**Interfaces:**
- Produces (file-static): `EmitTextRun(const glm::vec4& rect, const std::string& text, const std::string& fontName, float pixelSize, const glm::vec4& color, UIText::HAlign, UIText::VAlign, bool wrap, FontRegistry&, int& layer, std::vector<UiDrawCommand>&)`.

- [ ] **Step 1: Replace** the `EmitText` free function (lines 33–57) with a generic `EmitTextRun` plus a thin `EmitText` wrapper:

```cpp
	static void EmitTextRun(const glm::vec4& rect, const std::string& text, const std::string& fontName, float pixelSize, const glm::vec4& color, UIText::HAlign hAlign, UIText::VAlign vAlign, bool wrap, FontRegistry& fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		const FontAsset* font = fonts.Load(fontName);
		if (font == nullptr || font->atlasBindlessSlot == 0xFFFFFFFFu)
		{
			return;
		}
		const std::vector<ShapedGlyph> glyphs = ShapeText(*font, text, pixelSize, rect, wrap, static_cast<int>(hAlign), static_cast<int>(vAlign));
		for (const ShapedGlyph& glyph: glyphs)
		{
			UiDrawCommand cmd;
			cmd.data0 = glyph.rect;
			cmd.data1 = glyph.uv;
			cmd.color = color;
			cmd.type = kShapeSdfGlyph;
			cmd.layer = layer++;
			cmd.textureSlot = font->atlasBindlessSlot;
			out.push_back(cmd);
		}
	}

	static void EmitText(const UIRect& rect, const UIText& text, FontRegistry& fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		EmitTextRun(rect.resolvedRect, text.text, text.fontName, text.pixelSize, text.color, text.hAlign, text.vAlign, text.wrap, fonts, layer, out);
	}
```

- [ ] **Step 2: Build and run the existing UI draw tests** — a pure refactor must not change them.

Run: `cmake --build build/vs2022-msvc --target EngineTests --config Debug && ctest --test-dir build/vs2022-msvc -C Debug -R EngineTests --output-on-failure`
Expected: PASS (glyph tests unchanged).

- [ ] **Step 3: Commit.**

```bash
git add src/engine/ui/UiDrawBuilder.cpp
git commit -m "Extract EmitTextRun for reuse by widgets"
```

---

### Task 2.2: Emit widget draw commands

**Files:**
- Modify: `src/engine/ui/UiDrawBuilder.cpp`
- Test: `tests/ui/UiDrawBuilderTests.cpp`

**Interfaces:**
- Consumes: `EmitTextRun` (Task 2.1); `UISlider/UIToggle/UIButton/UIProgressBar` (1.1); `UISelectable` (focus read).

- [ ] **Step 1: Write failing tests** — append to `tests/ui/UiDrawBuilderTests.cpp`. A slider with value 0.5 emits track+fill+handle; the handle is a circle whose centre x sits mid-track; a progress bar emits track+fill.

```cpp
TEST_CASE("Builder emits track, fill, and handle for a UISlider")
{
	World w;
	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity s = w.Create();
	auto& sr = w.Emplace<ui::UIRect>(s);
	sr.resolvedRect = {100, 100, 200, 20};
	auto& slider = w.Emplace<ui::UISlider>(s);
	slider.minValue = 0.f;
	slider.maxValue = 1.f;
	slider.value = 0.5f;
	slider.handleRadius = 10.f;
	w.Emplace<HierarchyComponent>(s);
	ecs::SetParent(w, s, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 3);
	CHECK(cmds[0].type == ui::kShapeRect);   // track
	CHECK(cmds[1].type == ui::kShapeRect);   // fill
	CHECK(cmds[2].type == ui::kShapeCircle); // handle
	// inner track x0 = 102, inner width = 196, fill = 98 -> handle centre x = 200
	CHECK(cmds[2].data0.x == doctest::Approx(200.f));
	CHECK(cmds[2].data0.z == doctest::Approx(10.f)); // radius (unfocused)
}

TEST_CASE("Builder emits track and fill for a UIProgressBar")
{
	World w;
	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity p = w.Create();
	auto& pr = w.Emplace<ui::UIRect>(p);
	pr.resolvedRect = {0, 0, 200, 16};
	auto& bar = w.Emplace<ui::UIProgressBar>(p);
	bar.value = 0.25f;
	w.Emplace<HierarchyComponent>(p);
	ecs::SetParent(w, p, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 2);
	CHECK(cmds[0].type == ui::kShapeRect);
	CHECK(cmds[1].type == ui::kShapeRect);
	// inner width 196 * 0.25 = 49
	CHECK(cmds[1].data0.z == doctest::Approx(49.f));
}
```

- [ ] **Step 2: Run to confirm they fail.**

Run: `cmake --build build/vs2022-msvc --target EngineTests --config Debug && ctest --test-dir build/vs2022-msvc -C Debug -R EngineTests --output-on-failure`
Expected: FAIL — slider/progress emit nothing yet.

- [ ] **Step 3: Add emitters + a focus helper** in `UiDrawBuilder.cpp` (above `Walk`). Include `<algorithm>` and `<cmath>`.

```cpp
	static bool WidgetFocused(World& world, Entity e)
	{
		const auto* sel = world.TryGet<UISelectable>(e);
		return sel != nullptr && sel->focused;
	}

	static void EmitSlider(World& world, Entity e, const UIRect& rect, const UISlider& s, int& layer, std::vector<UiDrawCommand>& out)
	{
		const glm::vec4 r = rect.resolvedRect;
		const float range = std::max(s.maxValue - s.minValue, 1e-6f);
		const float t = std::clamp((s.value - s.minValue) / range, 0.f, 1.f);
		const float pad = 2.f;
		const float innerX = r.x + pad, innerY = r.y + pad;
		const float innerW = std::max(r.z - 2.f * pad, 0.f), innerH = std::max(r.w - 2.f * pad, 0.f);
		const float fillW = innerW * t;

		UiDrawCommand track;
		track.type = kShapeRect; track.data0 = r; track.data1.x = s.cornerRadius; track.color = s.trackColor; track.layer = layer++;
		out.push_back(track);

		UiDrawCommand fill;
		fill.type = kShapeRect; fill.data0 = {innerX, innerY, std::max(fillW, 1.f), innerH};
		fill.data1.x = std::max(s.cornerRadius - pad, 0.f); fill.color = s.fillColor; fill.layer = layer++;
		out.push_back(fill);

		const bool focused = WidgetFocused(world, e);
		UiDrawCommand handle;
		handle.type = kShapeCircle;
		handle.data0 = {innerX + fillW, r.y + r.w * 0.5f, s.handleRadius + (focused ? 2.f : 0.f), 0.f};
		handle.color = s.handleColor; handle.color.a *= s.pulse; handle.layer = layer++;
		out.push_back(handle);
	}

	static void EmitToggle(World& world, Entity e, const UIRect& rect, const UIToggle& tg, int& layer, std::vector<UiDrawCommand>& out)
	{
		const glm::vec4 r = rect.resolvedRect;
		UiDrawCommand track;
		track.type = kShapeRect; track.data0 = r;
		track.data1.x = std::min(tg.cornerRadius, r.w * 0.5f);
		track.color = tg.on ? tg.onColor : tg.trackColor; track.layer = layer++;
		out.push_back(track);

		const float pad = 2.f;
		const float leftX = r.x + pad + tg.knobRadius;
		const float rightX = r.x + r.z - pad - tg.knobRadius;
		const bool focused = WidgetFocused(world, e);
		UiDrawCommand knob;
		knob.type = kShapeCircle;
		knob.data0 = {tg.on ? rightX : leftX, r.y + r.w * 0.5f, tg.knobRadius + (focused ? 1.f : 0.f), 0.f};
		knob.color = tg.knobColor; knob.color.a *= tg.pulse; knob.layer = layer++;
		out.push_back(knob);
	}

	static void EmitProgressBar(const UIRect& rect, const UIProgressBar& p, int& layer, std::vector<UiDrawCommand>& out)
	{
		const glm::vec4 r = rect.resolvedRect;
		UiDrawCommand track;
		track.type = kShapeRect; track.data0 = r; track.data1.x = p.cornerRadius; track.color = p.trackColor; track.layer = layer++;
		out.push_back(track);

		const float pad = 2.f;
		const float innerW = std::max(r.z - 2.f * pad, 0.f);
		const float t = std::clamp(p.value, 0.f, 1.f);
		UiDrawCommand fill;
		fill.type = kShapeRect;
		fill.data0 = {r.x + pad, r.y + pad, std::max(innerW * t, 1.f), std::max(r.w - 2.f * pad, 0.f)};
		fill.data1.x = std::max(p.cornerRadius - pad, 0.f); fill.color = p.fillColor; fill.layer = layer++;
		out.push_back(fill);
	}

	static void EmitButton(World& world, Entity e, const UIRect& rect, const UIButton& b, FontRegistry* fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		const bool focused = WidgetFocused(world, e);
		UiDrawCommand bg;
		bg.type = kShapeRect; bg.data0 = rect.resolvedRect; bg.data1.x = b.cornerRadius;
		bg.color = focused ? b.bgColorFocused : b.bgColor; bg.layer = layer++;
		out.push_back(bg);
		if (fonts != nullptr)
		{
			EmitTextRun(rect.resolvedRect, b.label, b.fontName, b.pixelSize, focused ? b.textColorFocused : b.textColor, b.hAlign, b.vAlign, false, *fonts, layer, out);
		}
	}
```

- [ ] **Step 4: Call the emitters in `Walk`** — after the existing image/text block (after line 93, inside the `if (rect)` scope):

```cpp
			if (const auto* slider = world.TryGet<UISlider>(entity))
			{
				EmitSlider(world, entity, *rect, *slider, layer, out);
			}
			if (const auto* toggle = world.TryGet<UIToggle>(entity))
			{
				EmitToggle(world, entity, *rect, *toggle, layer, out);
			}
			if (const auto* bar = world.TryGet<UIProgressBar>(entity))
			{
				EmitProgressBar(*rect, *bar, layer, out);
			}
			if (fonts != nullptr)
			{
				if (const auto* button = world.TryGet<UIButton>(entity))
				{
					EmitButton(world, entity, *rect, *button, fonts, layer, out);
				}
			}
```

- [ ] **Step 5: Build + run tests.**

Run: `cmake --build build/vs2022-msvc --target EngineTests --config Debug && ctest --test-dir build/vs2022-msvc -C Debug -R EngineTests --output-on-failure`
Expected: PASS (the two new cases + all existing UI cases).

- [ ] **Step 6: Commit.**

```bash
git add src/engine/ui/UiDrawBuilder.cpp tests/ui/UiDrawBuilderTests.cpp
git commit -m "Render UI widgets in the draw builder"
```

---

### Task 2.3: Widget preview in the UI Canvas editor panel

**Files:**
- Modify: `src/app/debug/UiCanvasPanel.cpp` (`DrawPreviewElement`, ~line 784)

**Interfaces:**
- Consumes: `UiElement` (has `.entity`, `.min`, `.max` in screen px), `ImDrawList`, `world.TryGet<...>`.

- [ ] **Step 1: In `DrawPreviewElement`**, after the existing `UIImage`/`UIText` handling, add widget rendering using `element.min`/`element.max` and `drawList` primitives. Match the emitter geometry closely enough to author against.

```cpp
			if (const auto* slider = world.TryGet<ui::UISlider>(element.entity))
			{
				const float range = std::max(slider->maxValue - slider->minValue, 1e-6f);
				const float t = std::clamp((slider->value - slider->minValue) / range, 0.f, 1.f);
				drawList->AddRectFilled(element.min, element.max, ToU32(slider->trackColor), slider->cornerRadius * zoom);
				const float w = (element.max.x - element.min.x) * t;
				drawList->AddRectFilled(element.min, ImVec2(element.min.x + w, element.max.y), ToU32(slider->fillColor), 0.f);
				const float cy = (element.min.y + element.max.y) * 0.5f;
				drawList->AddCircleFilled(ImVec2(element.min.x + w, cy), slider->handleRadius * zoom, ToU32(slider->handleColor));
			}
			if (const auto* toggle = world.TryGet<ui::UIToggle>(element.entity))
			{
				drawList->AddRectFilled(element.min, element.max, ToU32(toggle->on ? toggle->onColor : toggle->trackColor), toggle->cornerRadius * zoom);
				const float cy = (element.min.y + element.max.y) * 0.5f;
				const float kx = toggle->on ? (element.max.x - toggle->knobRadius * zoom) : (element.min.x + toggle->knobRadius * zoom);
				drawList->AddCircleFilled(ImVec2(kx, cy), toggle->knobRadius * zoom, ToU32(toggle->knobColor));
			}
			if (const auto* bar = world.TryGet<ui::UIProgressBar>(element.entity))
			{
				drawList->AddRectFilled(element.min, element.max, ToU32(bar->trackColor), bar->cornerRadius * zoom);
				const float w = (element.max.x - element.min.x) * std::clamp(bar->value, 0.f, 1.f);
				drawList->AddRectFilled(element.min, ImVec2(element.min.x + w, element.max.y), ToU32(bar->fillColor), 0.f);
			}
			if (const auto* button = world.TryGet<ui::UIButton>(element.entity))
			{
				drawList->AddRectFilled(element.min, element.max, ToU32(button->bgColor), button->cornerRadius * zoom);
				if (!button->label.empty())
				{
					const ImVec2 ts = ImGui::CalcTextSize(button->label.c_str());
					const ImVec2 c((element.min.x + element.max.x - ts.x) * 0.5f, (element.min.y + element.max.y - ts.y) * 0.5f);
					drawList->AddText(c, ToU32(button->textColor), button->label.c_str());
				}
			}
```

Verify `ToU32(glm::vec4)`, `zoom`, and `std::clamp` availability in this TU (add `<algorithm>` if needed); match the existing `UIImage` branch's helpers.

- [ ] **Step 2: Build the editor.**

Run: `cmake --build build/vs2022-msvc --target Editor --config Debug`
Expected: builds clean.

- [ ] **Step 3: Commit.**

```bash
git add src/app/debug/UiCanvasPanel.cpp
git commit -m "Preview UI widgets in the canvas panel"
```

---

## Phase 3 — Interaction (UiWidgetSystem)

### Task 3.1: Pure value helpers (TDD)

**Files:**
- Create: `src/engine/ui/UiWidgetSystem.hpp`
- Create: `src/engine/ui/UiWidgetSystem.cpp`
- Create: `tests/ui/UiWidgetSystemTests.cpp`

**Interfaces:**
- Produces: `float aether::ui::SliderNormalized(float, float, float)`, `SliderQuantize(float value, float min, float max, float step)`, `SliderValueFromMouseX(float mouseX, const glm::vec4& trackRect, float min, float max, float step)`, and `class UiWidgetSystem { static void Update(World&, Input&, float time); };` (implemented in 3.2).

- [ ] **Step 1: Write the header** `UiWidgetSystem.hpp`:

```cpp
#pragma once

#include <glm/glm.hpp>

namespace aether
{
	class World;
	class Input;
} // namespace aether

namespace aether::ui
{
	// Pure helpers (unit-tested); no ECS/Input deps.
	float SliderNormalized(float value, float minValue, float maxValue);
	float SliderQuantize(float value, float minValue, float maxValue, float step);
	float SliderValueFromMouseX(float mouseX, const glm::vec4& trackRect, float minValue, float maxValue, float step);

	// Drives interactive widgets from focus (UiNavigationSystem) + Input. Runs after nav,
	// before scripts. Reads/writes UISlider/UIToggle runtime state.
	class UiWidgetSystem
	{
	public:
		static void Update(World& world, Input& input, float time);
	};
} // namespace aether::ui
```

- [ ] **Step 2: Write failing tests** `tests/ui/UiWidgetSystemTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "ui/UiWidgetSystem.hpp"

using namespace aether;

TEST_CASE("SliderNormalized clamps to 0..1")
{
	CHECK(ui::SliderNormalized(0.5f, 0.f, 1.f) == doctest::Approx(0.5f));
	CHECK(ui::SliderNormalized(-1.f, 0.f, 1.f) == doctest::Approx(0.f));
	CHECK(ui::SliderNormalized(5.f, 0.f, 10.f) == doctest::Approx(0.5f));
	CHECK(ui::SliderNormalized(2.f, 0.f, 0.f) == doctest::Approx(0.f)); // degenerate range
}

TEST_CASE("SliderQuantize snaps to the nearest step and clamps")
{
	CHECK(ui::SliderQuantize(0.52f, 0.f, 1.f, 0.05f) == doctest::Approx(0.5f));
	CHECK(ui::SliderQuantize(0.53f, 0.f, 1.f, 0.05f) == doctest::Approx(0.55f));
	CHECK(ui::SliderQuantize(1.4f, 0.f, 1.f, 0.05f) == doctest::Approx(1.f));   // clamp high
	CHECK(ui::SliderQuantize(-0.4f, 0.f, 1.f, 0.05f) == doctest::Approx(0.f));  // clamp low
	CHECK(ui::SliderQuantize(0.377f, 0.f, 1.f, 0.f) == doctest::Approx(0.377f)); // continuous
}

TEST_CASE("SliderValueFromMouseX maps track-relative x to a stepped value")
{
	const glm::vec4 track{100.f, 0.f, 204.f, 20.f}; // pad 2 -> innerX 102, innerW 200
	CHECK(ui::SliderValueFromMouseX(102.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(0.f));
	CHECK(ui::SliderValueFromMouseX(302.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(1.f));
	CHECK(ui::SliderValueFromMouseX(202.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(0.5f));
	CHECK(ui::SliderValueFromMouseX(50.f, track, 0.f, 1.f, 0.05f) == doctest::Approx(0.f)); // left of track
}
```

- [ ] **Step 3: Implement the helpers** in `UiWidgetSystem.cpp` (leave `Update` as a stub for now so it links):

```cpp
#include "ui/UiWidgetSystem.hpp"

#include <algorithm>
#include <cmath>

namespace aether::ui
{
	float SliderNormalized(float value, float minValue, float maxValue)
	{
		const float range = maxValue - minValue;
		if (range <= 1e-6f)
		{
			return 0.f;
		}
		return std::clamp((value - minValue) / range, 0.f, 1.f);
	}

	float SliderQuantize(float value, float minValue, float maxValue, float step)
	{
		float v = std::clamp(value, minValue, maxValue);
		if (step > 1e-6f)
		{
			v = minValue + std::round((v - minValue) / step) * step;
			v = std::clamp(v, minValue, maxValue);
		}
		return v;
	}

	float SliderValueFromMouseX(float mouseX, const glm::vec4& trackRect, float minValue, float maxValue, float step)
	{
		const float pad = 2.f;
		const float innerX = trackRect.x + pad;
		const float innerW = std::max(trackRect.z - 2.f * pad, 1e-6f);
		const float t = std::clamp((mouseX - innerX) / innerW, 0.f, 1.f);
		return SliderQuantize(minValue + t * (maxValue - minValue), minValue, maxValue, step);
	}

	void UiWidgetSystem::Update(World&, Input&, float) {}
} // namespace aether::ui
```

- [ ] **Step 4: Build + run tests.**

Run: `cmake --build build/vs2022-msvc --target EngineTests --config Debug && ctest --test-dir build/vs2022-msvc -C Debug -R EngineTests --output-on-failure`
Expected: PASS (the new helper cases).

- [ ] **Step 5: Commit.**

```bash
git add src/engine/ui/UiWidgetSystem.hpp src/engine/ui/UiWidgetSystem.cpp tests/ui/UiWidgetSystemTests.cpp
git commit -m "Add UiWidgetSystem value helpers"
```

---

### Task 3.2: `UiWidgetSystem::Update` + nav arbitration + wiring

**Files:**
- Modify: `src/engine/ui/UiWidgetSystem.cpp`
- Modify: `src/engine/ui/UiNavigationSystem.cpp`
- Modify: `src/app/systems/ScriptComponentSystem.cpp`

**Interfaces:**
- Consumes: helpers (3.1); `UISlider/UIToggle/UISelectable`; `Input` (`GetMousePos`, `IsMouseButtonDown`, `IsKeyPressed`); `ecs::HasDisabledAncestor`.

- [ ] **Step 1: Implement `Update`** in `UiWidgetSystem.cpp`. Add includes: `<entt/entt.hpp>`, `platform/Input.hpp`, `scene/Entity.hpp`, `scene/Hierarchy.hpp`, `scene/World.hpp`, `ui/UiComponents.hpp`.

```cpp
	namespace
	{
		bool Focused(World& world, Entity e)
		{
			const auto* s = world.TryGet<UISelectable>(e);
			return s != nullptr && s->focused;
		}

		bool Activated(World& world, Entity e)
		{
			const auto* s = world.TryGet<UISelectable>(e);
			return s != nullptr && s->activated;
		}

		float FocusPulse(float time)
		{
			return 0.7f + 0.3f * std::sin(time * 4.5f);
		}
	} // namespace

	void UiWidgetSystem::Update(World& world, Input& input, float time)
	{
		const glm::vec2 mouse = input.GetMousePos();
		const bool mouseDown = input.IsMouseButtonDown(MouseButton::Left);
		const bool right = input.IsKeyPressed(Key::Right);
		const bool left = input.IsKeyPressed(Key::Left);

		world.View<UISlider, UIRect>().each(
		        [&](entt::entity ent, UISlider& s, UIRect& rect)
		        {
			        const Entity e = World::FromEntt(ent);
			        s.changed = false;
			        if (ecs::HasDisabledAncestor(world, e))
			        {
				        s.dragging = false;
				        return;
			        }
			        const bool focused = Focused(world, e);
			        s.pulse = focused ? FocusPulse(time) : 1.f;

			        if (focused && (right || left))
			        {
				        const float delta = (s.step > 1e-6f ? s.step : (s.maxValue - s.minValue) * 0.02f) * (right ? 1.f : -1.f);
				        const float nv = SliderQuantize(s.value + delta, s.minValue, s.maxValue, s.step);
				        if (nv != s.value)
				        {
					        s.value = nv;
					        s.changed = true;
				        }
			        }

			        const glm::vec4 r = rect.resolvedRect;
			        const bool hover = mouse.x >= r.x && mouse.x <= r.x + r.z && mouse.y >= r.y && mouse.y <= r.y + r.w;
			        if (mouseDown && (s.dragging || hover))
			        {
				        s.dragging = true;
				        const float nv = SliderValueFromMouseX(mouse.x, r, s.minValue, s.maxValue, s.step);
				        if (nv != s.value)
				        {
					        s.value = nv;
					        s.changed = true;
				        }
			        }
			        if (!mouseDown)
			        {
				        s.dragging = false;
			        }
		        });

		world.View<UIToggle, UIRect>().each(
		        [&](entt::entity ent, UIToggle& tg, UIRect&)
		        {
			        const Entity e = World::FromEntt(ent);
			        tg.changed = false;
			        if (ecs::HasDisabledAncestor(world, e))
			        {
				        return;
			        }
			        tg.pulse = Focused(world, e) ? FocusPulse(time) : 1.f;
			        if (Activated(world, e))
			        {
				        tg.on = !tg.on;
				        tg.changed = true;
			        }
		        });
	}
```

- [ ] **Step 2: Nav arbitration** in `UiNavigationSystem.cpp`. A focused horizontal slider captures `←`/`→`. In the keyboard block (lines 134–150), gate the Right/Left branches so up/down still navigate:

```cpp
			const bool focusedIsSlider = world.TryGet<UISlider>(focused) != nullptr;
			glm::vec2 dir{0.0f, 0.0f};
			if (input.IsKeyPressed(Key::Down))
			{
				dir = {0.0f, 1.0f};
			}
			else if (input.IsKeyPressed(Key::Up))
			{
				dir = {0.0f, -1.0f};
			}
			else if (input.IsKeyPressed(Key::Right) && !focusedIsSlider)
			{
				dir = {1.0f, 0.0f};
			}
			else if (input.IsKeyPressed(Key::Left) && !focusedIsSlider)
			{
				dir = {-1.0f, 0.0f};
			}
```

- [ ] **Step 3: Wire the system** into `ScriptComponentSystem.cpp`. Add `#include "ui/UiWidgetSystem.hpp"` near the nav include (line 13), and call it right after nav (line 220):

```cpp
		if (sceneCtx->input != nullptr)
		{
			aether::ui::UiNavigationSystem::Update(world, *sceneCtx->input);
			aether::ui::UiWidgetSystem::Update(world, *sceneCtx->input, static_cast<float>(sceneCtx->elapsedTime));
		}
```

(`sceneCtx->elapsedTime` accumulates just below in the same function; passing the pre-increment value is fine for the pulse phase.)

- [ ] **Step 4: Build the editor.**

Run: `cmake --build build/vs2022-msvc --target Editor --config Debug`
Expected: builds clean.

- [ ] **Step 5: Commit.**

```bash
git add src/engine/ui/UiWidgetSystem.cpp src/engine/ui/UiNavigationSystem.cpp src/app/systems/ScriptComponentSystem.cpp
git commit -m "Drive UI widgets from input via UiWidgetSystem"
```

---

## Phase 4 — C# API

### Task 4.1: Native exports + P/Invoke declarations

**Files:**
- Modify: `src/app/scripting/interop/UiExports.cpp`
- Modify: `managed/AetherCore/Internal/Native.cs`

**Interfaces:**
- Produces (C ABI): `aether_ui_get_slider_value`/`set_slider_value`, `get_toggle`/`set_toggle`, `get_progress`/`set_progress`, `get_button_label`/`set_button_label`, `was_changed`.

- [ ] **Step 1: Add exports** to `UiExports.cpp` (before the final closing brace). Follow the existing `TryGet<...>` pattern.

```cpp
// ── Widgets ─────────────────────────────────────────────────────────────────────
AE_SCRIPT_API float aether_ui_get_slider_value(std::uint32_t id)
{
	const auto* s = ActiveWorld().TryGet<aether::ui::UISlider>(aether::Entity{id});
	return s != nullptr ? s->value : 0.f;
}

AE_SCRIPT_API void aether_ui_set_slider_value(std::uint32_t id, float value)
{
	if (auto* s = ActiveWorld().TryGet<aether::ui::UISlider>(aether::Entity{id}))
	{
		s->value = std::clamp(value, s->minValue, s->maxValue);
	}
}

AE_SCRIPT_API std::int32_t aether_ui_get_toggle(std::uint32_t id)
{
	const auto* t = ActiveWorld().TryGet<aether::ui::UIToggle>(aether::Entity{id});
	return (t != nullptr && t->on) ? 1 : 0;
}

AE_SCRIPT_API void aether_ui_set_toggle(std::uint32_t id, std::int32_t on)
{
	if (auto* t = ActiveWorld().TryGet<aether::ui::UIToggle>(aether::Entity{id}))
	{
		t->on = (on != 0);
	}
}

AE_SCRIPT_API float aether_ui_get_progress(std::uint32_t id)
{
	const auto* p = ActiveWorld().TryGet<aether::ui::UIProgressBar>(aether::Entity{id});
	return p != nullptr ? p->value : 0.f;
}

AE_SCRIPT_API void aether_ui_set_progress(std::uint32_t id, float value)
{
	if (auto* p = ActiveWorld().TryGet<aether::ui::UIProgressBar>(aether::Entity{id}))
	{
		p->value = std::clamp(value, 0.f, 1.f);
	}
}

AE_SCRIPT_API std::int32_t aether_ui_get_button_label(std::uint32_t id, char* buf, std::int32_t bufLen)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UIButton>(aether::Entity{id});
	if (b == nullptr || buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(b->label.size()));
	std::memcpy(buf, b->label.data(), static_cast<std::size_t>(n));
	return n;
}

AE_SCRIPT_API void aether_ui_set_button_label(std::uint32_t id, const char* text)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UIButton>(aether::Entity{id}))
	{
		b->label = text != nullptr ? text : "";
	}
}

// True if a UISlider or UIToggle on this entity changed by user input this frame.
AE_SCRIPT_API std::int32_t aether_ui_was_changed(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (const auto* s = world.TryGet<aether::ui::UISlider>(e); s != nullptr && s->changed)
	{
		return 1;
	}
	if (const auto* t = world.TryGet<aether::ui::UIToggle>(e); t != nullptr && t->changed)
	{
		return 1;
	}
	return 0;
}
```

- [ ] **Step 2: Add P/Invoke decls** to `Native.cs`, after `aether_ui_set_interactable` (line 149). Match the file's `[LibraryImport(Lib)]` style; the label setter needs UTF-8 marshalling.

```csharp
    [LibraryImport(Lib)]
    internal static partial float aether_ui_get_slider_value(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_slider_value(uint id, float value);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_toggle(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_toggle(uint id, int on);

    [LibraryImport(Lib)]
    internal static partial float aether_ui_get_progress(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_progress(uint id, float value);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_button_label(uint id, byte* buf, int bufLen);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_button_label(uint id, string text);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_was_changed(uint id);
```

- [ ] **Step 3: Build editor + managed assemblies.**

Run: `cmake --build build/vs2022-msvc --target Editor --config Debug && cmake --build build/vs2022-msvc --target ManagedAssemblies --config Debug`
Expected: builds clean.

- [ ] **Step 4: Commit.**

```bash
git add src/app/scripting/interop/UiExports.cpp managed/AetherCore/Internal/Native.cs
git commit -m "Export UI widget accessors to C#"
```

---

### Task 4.2: `Ui.cs` public API

**Files:**
- Modify: `managed/AetherCore/Ui.cs`

**Interfaces:**
- Produces: `Ui.GetSliderValue/SetSliderValue`, `GetToggle/SetToggle`, `GetProgress/SetProgress`, `GetButtonLabel/SetButtonLabel`, `WasChanged`.

- [ ] **Step 1: Add a Widgets region** to `Ui.cs` before the closing brace:

```csharp
    // ── Widgets (engine-drawn UISlider / UIToggle / UIButton / UIProgressBar) ──────

    /// <summary>Current slider value in its own units (min..max).</summary>
    public static float GetSliderValue(Entity e) => Native.aether_ui_get_slider_value(e.Id);

    /// <summary>Set a slider's value (clamped to its min..max).</summary>
    public static void SetSliderValue(Entity e, float value) => Native.aether_ui_set_slider_value(e.Id, value);

    /// <summary>Current toggle state.</summary>
    public static bool GetToggle(Entity e) => Native.aether_ui_get_toggle(e.Id) != 0;

    /// <summary>Set a toggle's state.</summary>
    public static void SetToggle(Entity e, bool on) => Native.aether_ui_set_toggle(e.Id, on ? 1 : 0);

    /// <summary>Current progress-bar fill (0..1).</summary>
    public static float GetProgress(Entity e) => Native.aether_ui_get_progress(e.Id);

    /// <summary>Set a progress-bar's fill (clamped 0..1).</summary>
    public static void SetProgress(Entity e, float value) => Native.aether_ui_set_progress(e.Id, value);

    /// <summary>A button's label text.</summary>
    public static unsafe string GetButtonLabel(Entity e)
    {
        Span<byte> buffer = stackalloc byte[256];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_ui_get_button_label(e.Id, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    /// <summary>Set a button's label (ASCII only).</summary>
    public static void SetButtonLabel(Entity e, string label) => Native.aether_ui_set_button_label(e.Id, label);

    /// <summary>True on the frame a slider or toggle on this entity was changed by the user
    /// (keyboard/drag/activation). Poll this to persist settings.</summary>
    public static bool WasChanged(Entity e) => Native.aether_ui_was_changed(e.Id) != 0;
```

- [ ] **Step 2: Build managed assemblies.**

Run: `cmake --build build/vs2022-msvc --target ManagedAssemblies --config Debug`
Expected: builds clean.

- [ ] **Step 3: Commit.**

```bash
git add managed/AetherCore/Ui.cs
git commit -m "Add Ui widget value API"
```

---

## Phase 5 — Migration & Verification

### Task 5.1: Rebuild the Settings screen on widgets

Replace the `Slider{Music,Sfx,InkGlow}{Track,Fill,Orb}` triples with three `UISlider`, `Shake{Socket,Orb,Value}` with one `UIToggle`, and `SettingsBack` (UIText) with a `UIButton`. Static row labels ("music volume", etc.) stay.

**Files:**
- Modify: `projects/INKBOUND/scenes/Menu.scene.toml` (via MCP + `save_scene`)
- Rewrite: `projects/INKBOUND/scripts/SettingsScreen.cs`

**Interfaces:**
- Consumes: `Ui.GetSliderValue/SetSliderValue/GetToggle/SetToggle/WasChanged/WasActivated/SetFocus`.
- Names the script finds: `SettingsBack`, `SliderMusic`, `SliderSfx`, `SliderInkGlow`, `ShakeToggle`.

- [ ] **Step 1: Clear the stale managed cache and restart the editor** (SDK changed in Phase 4):

```bash
rm -rf projects/INKBOUND/Builds/Intermediate/managed projects/INKBOUND/scripts/obj projects/INKBOUND/scripts/bin
```
Stop `Editor.exe`, relaunch on `AETHER_CONTROL_PORT=8787`, `open_project` INKBOUND, `load_scene` Menu.

- [ ] **Step 2: Author the widgets via MCP.** Under `SettingsRoot`, delete the six slider part-entities + three shake part-entities, and add:
  - `SliderMusic`, `SliderSfx`, `SliderInkGlow` — "UI Slider" (`add_components` with values, which fires `postSet` → composes `UIRect`+`UISelectable`). Set each `UI Rect` to the row's bar position (reuse the old `*Track` offsets). Fields: `min 0`, `max 1`, `step 0.05`, INKBOUND colors (`fill_color`/`handle_color` = accent cyan `[0.302,0.851,1.0,1.0]`, `track_color` = `[0.10,0.11,0.14,1]`).
  - `ShakeToggle` — "UI Toggle" at the old socket position; `on_color`/`knob` accent-tinted.
  - Convert `SettingsBack` to a "UI Button": `label "< back to the dark"` (ASCII), `font IBMPlexMono-Italic`, `text_color [0.337,0.361,0.431,1]`, `text_color_focused` accent, transparent `bg_color`/`bg_color_focused` (`a=0`).
  - `save_scene`.

- [ ] **Step 3: Rewrite `SettingsScreen.cs`** — the widgets own geometry/interaction now, so the script only seeds values and mirrors changes:

```csharp
using System.Numerics;
using AetherCore;

namespace AetherGame;

/// <summary>Settings ("attune"). Sliders and the toggle are engine UI widgets now: the nav
/// system moves focus, UiWidgetSystem adjusts the focused slider (left/right + mouse drag) and
/// flips the toggle on activation. This script seeds the widgets from GameSettings, mirrors
/// user changes back, and persists. Ink Glow feeds InkBlock; Music/SFX persist for a future
/// audio system.</summary>
public sealed class SettingsScreen : EntityScript, IMenuScreen
{
    private Entity _back, _music, _sfx, _ink, _shake;

    public override void OnAttach()
    {
        ScreenRegistry<SettingsScreen>.Register("SettingsRoot", this);
        _back  = Scene.Find("SettingsBack");
        _music = Scene.Find("SliderMusic");
        _sfx   = Scene.Find("SliderSfx");
        _ink   = Scene.Find("SliderInkGlow");
        _shake = Scene.Find("ShakeToggle");
    }

    public void OnShown()
    {
        if (_music.IsValid) Ui.SetSliderValue(_music, GameSettings.MusicVolume);
        if (_sfx.IsValid)   Ui.SetSliderValue(_sfx, GameSettings.SfxVolume);
        if (_ink.IsValid)   Ui.SetSliderValue(_ink, GameSettings.InkGlow);
        if (_shake.IsValid) Ui.SetToggle(_shake, GameSettings.ScreenShake);
        if (_music.IsValid) Ui.SetFocus(_music);
    }

    public void HandleInput()
    {
        if (_back.IsValid && Ui.WasActivated(_back)) { MenuController.Instance?.Go(MenuScreen.Title); return; }

        bool dirty = false;
        if (_music.IsValid && Ui.WasChanged(_music)) { GameSettings.MusicVolume = Ui.GetSliderValue(_music); dirty = true; }
        if (_sfx.IsValid   && Ui.WasChanged(_sfx))   { GameSettings.SfxVolume   = Ui.GetSliderValue(_sfx);   dirty = true; }
        if (_ink.IsValid   && Ui.WasChanged(_ink))   { GameSettings.InkGlow     = Ui.GetSliderValue(_ink);   dirty = true; }
        if (_shake.IsValid && Ui.WasChanged(_shake)) { GameSettings.ScreenShake = Ui.GetToggle(_shake);      dirty = true; }
        if (dirty) GameSettings.Save();
    }

    public override void OnUpdate(float dt) { }
}
```

- [ ] **Step 4: Play-test via MCP.** `play`; `play_input_sequence` to move focus down the sliders, adjust with `←`/`→`, toggle the shake with Enter, and activate back. `screenshot` and verify: fills/handles move, toggle knob slides, `GameSettings` persists (re-enter Settings → values restored), back returns to Title. `stop`.

- [ ] **Step 5: Commit.**

```bash
git add projects/INKBOUND/scenes/Menu.scene.toml projects/INKBOUND/scripts/SettingsScreen.cs
git commit -m "Rebuild INKBOUND Settings on UI widgets"
```

---

### Task 5.2: Convert Title menu items to buttons

**Files:**
- Modify: `projects/INKBOUND/scenes/Menu.scene.toml` (via MCP + `save_scene`)
- Modify: `projects/INKBOUND/scripts/TitleScreen.cs`

**Interfaces:**
- Consumes: `Ui.WasActivated`, `Ui.SetFocus`.

- [ ] **Step 1: Read the current `TitleScreen.cs`** to capture the exact item entity names and each item's action (descend → Level Select, attune → Settings, release → quit-intent), and its current per-frame focus-styling loop.

- [ ] **Step 2: Convert the item entities via MCP.** For each menu item under `TitleRoot`, add a "UI Button" with `label` = the ASCII item text, `font PixelStorm` (or the current heading/menu font), `text_color` = the muted default, `text_color_focused` = accent cyan, transparent `bg_color`/`bg_color_focused` (`a=0`), `h_align left`. Remove the now-redundant separate `UIText`/`UISelectable` styling parts if the item was a bare selectable+text. `save_scene`.

- [ ] **Step 3: Simplify `TitleScreen.cs`** — delete the per-frame focus-styling loop (the button renders its own focus colors); keep only `OnShown` (`Ui.SetFocus` the first item) and `HandleInput` (`Ui.WasActivated(item)` → the existing action per item). Preserve any title flicker/atmosphere unrelated to the items.

- [ ] **Step 4: Rebuild managed + play-test.**

```bash
rm -rf projects/INKBOUND/Builds/Intermediate/managed projects/INKBOUND/scripts/obj projects/INKBOUND/scripts/bin
```
`play`; arrow-key between items, verify focus color is engine-driven and activation still routes (descend→Level Select, attune→Settings); `stop`.

- [ ] **Step 5: Commit.**

```bash
git add projects/INKBOUND/scenes/Menu.scene.toml projects/INKBOUND/scripts/TitleScreen.cs
git commit -m "Convert INKBOUND title items to UI buttons"
```

---

### Task 5.3: Progress-bar round-trip + full walkthrough

**Files:** none (verification only).

- [ ] **Step 1: Progress-bar MCP round-trip.** On a scratch canvas (or a temp entity in Menu), `add_component` "UI Progress Bar", `set_component` `value 0.6` + accent colors, size its `UI Rect`. `screenshot` the UI Canvas panel and the viewport; confirm a 60%-filled bar renders in both. Delete the scratch entity (do not save it into Menu).

- [ ] **Step 2: Full menu walkthrough.** From Title: descend → Level Select → back; attune → Settings → adjust each slider + toggle + back; confirm no console errors (`get_console_log`), stable FPS, and persisted settings across a stop/relaunch. `stop`.

- [ ] **Step 3: Run the full test suite once more.**

Run: `ctest --test-dir build/vs2022-msvc -C Debug -R EngineTests --output-on-failure`
Expected: PASS.

---

## Completion

After all tasks: announce use of **superpowers:finishing-a-development-branch**, verify the full `EngineTests` suite is green, then fast-forward merge + push to `master` (the working branch), per the always-merge-to-master rule.

## Self-Review Notes

- **Spec coverage:** all four components (1.1–1.2), engine rendering (2.1–2.3), interaction + nav arbitration (3.1–3.2), C# API (4.1–4.2), Settings + Title migration (5.1–5.2), progress-bar round-trip (5.3) — every spec section maps to a task. Level Select migration is intentionally deferred per the spec.
- **Deviation from spec:** the handle/knob focus "pulse" is driven by `UiWidgetSystem` writing a runtime `pulse` field that the (stateless) draw builder multiplies into alpha — the builder never sees time directly. Same visible result as the spec's per-script `Sin` pulse, without coupling the builder to a clock.
- **Type consistency:** `WasChanged` covers slider+toggle (matches `aether_ui_was_changed`); `pixelSize` (not `fontSize`) is used consistently on `UIButton` to mirror `UIText`; helper names `SliderNormalized/SliderQuantize/SliderValueFromMouseX` are identical across header, impl, tests, and callers.
