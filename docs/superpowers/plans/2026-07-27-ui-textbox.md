# UI Text Box + Clipping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a full-featured single-line text input widget to the in-game UI system, plus the general rect-clipping mechanism it needs, so an ENet connect screen can take an IP:port.

**Architecture:** Three layers. (1) A clip rect on `UiDrawCommand`, honoured by a fragment discard, driven by a clip stack in `UiDrawBuilder` and exposed as a `UIMask` component. (2) A pure, dependency-free text-editing core (`UiTextEdit`) unit-tested without `World`, `Input` or GLFW — the same idiom `SliderQuantize` already uses. (3) A thin `UiTextBoxSystem` marshalling ECS + `Input` into that core, with a `UIKeyboardCapture` marker component that tells `UiNavigationSystem` to yield its keys.

**Tech Stack:** C++20, EnTT, GLM, GLFW, Slang shaders, doctest, CMake/Ninja, C#/CoreCLR interop.

**Spec:** `docs/superpowers/specs/2026-07-27-ui-textbox-design.md`

## Global Constraints

- **ASCII only.** `ShapeText` treats each `char` byte as a codepoint (`FontRegistry.cpp:58`) and the baked atlases are ASCII. Insertion filters to printable ASCII `0x20`–`0x7E`; caret indices are byte indices and byte == char.
- **Single line.** No multiline, no wrap inside the field, no IME preedit, no bidi.
- **Code style:** tabs for indentation, Allman braces, `m_` member prefix, `namespace aether::ui`, `[[nodiscard]]` on pure queries. Match surrounding code.
- **Commit style:** plain imperative subject, no `feat:`/`fix:` prefixes, no attribution trailers, no emoji. Body only when several changes are grouped.
- **`UiDrawCommand` field order is load-bearing** — it must stay byte-for-byte identical to `DrawCommandData` in `src/shaders/include/UIStructs.slangh` (std430).
- **Build tree:** `build/ninja-clang` (Ninja, RelWithDebInfo). Engine sources and test sources are both globbed with `CONFIGURE_DEPENDS`, so new files under `src/engine/` and `tests/` need no CMake edit — but the first build after adding one triggers a reconfigure.
- **Shader changes** need the `CompileShaders` target and an `EngineAssetsPak` rebuild before they take effect at runtime.
- Run built executables from the build-tree root (`shaders://` is CWD-relative).

## File Structure

**Created**
- `src/engine/ui/UiTextEdit.hpp` / `.cpp` — pure text-editing core. No engine dependencies beyond `FontAsset`.
- `src/engine/ui/UiTextBoxSystem.hpp` / `.cpp` — per-frame marshaller: ECS + `Input` into the core.
- `src/shaders/include/UiVertexOut.slangh` — the shared UI `VSOutput` struct + `UiClipDiscard` helper.
- `tests/ui/UiTextEditTests.cpp` — the core's unit tests.

**Modified**
- `src/engine/ui/UiDrawCommand.hpp` — `clipRect` field, `kFlagClip`.
- `src/engine/ui/UiComponents.hpp` — `UITextBox`, `UIMask`, `UIKeyboardCapture`.
- `src/engine/ui/UiDrawBuilder.cpp` — clip stack, `UIMask` handling, `EmitTextBox`.
- `src/engine/ui/UiNavigationSystem.cpp` — capture yields nav keys; Tab moves focus.
- `src/engine/ui/UiEntities.hpp` / `.cpp` — `CreateTextBoxEntity`.
- `src/engine/platform/Input.hpp` / `.cpp` — synthetic typed chars, clipboard.
- `src/shaders/include/UIStructs.slangh`, `src/shaders/ui_shapes.slang`, `src/shaders/ui_build_draws.slang`.
- `projects/INKBOUND/assets/shaders/{ui_glitch_text,ui_dialogue_text,ui_ink_ui,ui_inkwell}.slang` — migrate to the shared header.
- `src/app/systems/ScriptComponentSystem.cpp` — tick the new system.
- `src/app/scene/reflection/MoreComponents.reflect.cpp` — two `AE_COMPONENT` blocks.
- `src/app/scripting/interop/UiExports.cpp`, `managed/AetherCore/Internal/Native.cs`, `managed/AetherCore/Ui.cs`, `managed/AetherCore/Input.cs`.
- `src/app/editor/ControlMethods.cpp` — `send_input` gains `text`.
- `src/app/debug/ComponentDrawers.cpp`, `src/app/debug/UiCanvasPanel.cpp` — editor summaries.
- `tests/ui/UiDrawBuilderTests.cpp` — clip + mask coverage.

---

### Task 1: Clip field on the draw command

Adds the data channel and the GPU-side test. No behaviour change yet: nothing sets the flag, so every command stays unclipped.

**Files:**
- Modify: `src/engine/ui/UiDrawCommand.hpp`
- Modify: `src/shaders/include/UIStructs.slangh`
- Modify: `src/shaders/ui_build_draws.slang:28`
- Test: `tests/ui/UiDrawBuilderTests.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `aether::ui::kFlagClip` (`std::uint32_t`, value `1u << 2`); `UiDrawCommand::clipRect` (`glm::vec4`, `(x, y, w, h)` in pixels); `sizeof(UiDrawCommand) == 80`.

- [ ] **Step 1: Write the failing test**

Append to `tests/ui/UiDrawBuilderTests.cpp`:

```cpp
TEST_CASE("Draw commands are unclipped by default")
{
	static_assert(sizeof(ui::UiDrawCommand) == 80, "must match DrawCommandData std430 layout");

	const ui::UiDrawCommand cmd;
	CHECK((cmd.flags & ui::kFlagClip) == 0u);
	CHECK(cmd.clipRect.x == doctest::Approx(0.f));
	CHECK(cmd.clipRect.z == doctest::Approx(0.f));
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `kFlagClip` is not a member of `aether::ui`, and `clipRect` is not a member of `UiDrawCommand`.

- [ ] **Step 3: Add the field and flag**

In `src/engine/ui/UiDrawCommand.hpp`, after the `kFlagNoMaterial` definition:

```cpp
	// Clip rect in pixels, honoured only when this bit is set. A flag rather than a sentinel
	// so a zero-size clip legitimately hides content instead of reading as "unclipped".
	inline constexpr std::uint32_t kFlagClip = 1u << 2;
```

Replace the `UiDrawCommand` struct and its assertion with:

```cpp
	struct UiDrawCommand
	{
		glm::vec4 data0{0.f};
		glm::vec4 data1{0.f};
		glm::vec4 color{1.f};
		// (x, y, w, h) px. Read by the fragment stage only when kFlagClip is set; a fragment
		// whose SV_Position falls outside is discarded.
		glm::vec4 clipRect{0.f};
		// Field order below is load-bearing: it must stay byte-for-byte with DrawCommandData
		// in shaders/include/UIStructs.slangh (std430).
		std::uint32_t type = kShapeRect;
		std::int32_t layer = 0;
		std::uint32_t textureSlot = 0;
		std::uint32_t flags = 0;
	};

	static_assert(sizeof(UiDrawCommand) == 80, "must match DrawCommandData std430 layout");
```

- [ ] **Step 4: Mirror it in the shader struct**

In `src/shaders/include/UIStructs.slangh`, inside `DrawCommandData`, after `float4 color;`:

```hlsl
    float4 clipRect;     // x,y,w,h px; honoured only when (flags & 4) is set
```

- [ ] **Step 5: Record the groupshared cost**

In `src/shaders/ui_build_draws.slang`, replace line 28 with:

```hlsl
// 256 x sizeof(DrawCommandData) = 256 x 80 = 20 KB of groupshared, against a 32 KB floor.
// Growing DrawCommandData past 128 bytes/element would exceed it - size the sort, don't assume.
groupshared DrawCommandData g_LocalData[kMaxLocalSort];
```

- [ ] **Step 6: Run tests and compile shaders**

```bash
cmake --build build/ninja-clang --target EngineTests CompileShaders
```

Then:

```bash
./build/ninja-clang/tests/EngineTests.exe -tc="Draw commands are unclipped by default"
```

Expected: PASS, and `CompileShaders` succeeds (the struct grew on both sides together).

- [ ] **Step 7: Run the whole suite**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all tests pass — nothing reads `clipRect` yet.

- [ ] **Step 8: Commit**

```bash
git add src/engine/ui/UiDrawCommand.hpp src/shaders/include/UIStructs.slangh src/shaders/ui_build_draws.slang tests/ui/UiDrawBuilderTests.cpp
git commit -m "Add a clip rect to the UI draw command"
```

---

### Task 2: Shared VSOutput header and project shader migration

Project material shaders hand-copy `ui_shapes.slang`'s `VSOutput` and must match it exactly. Task 3 adds a varying, which would silently corrupt them. Move the struct into a shared header first and migrate every copy — turning a silent breakage trap into a compile-time contract.

**Files:**
- Create: `src/shaders/include/UiVertexOut.slangh`
- Modify: `src/shaders/ui_shapes.slang:28-39` (struct), `:111-127` (fragment entry)
- Modify: `projects/INKBOUND/assets/shaders/ui_glitch_text.slang`, `ui_dialogue_text.slang`, `ui_ink_ui.slang`, `ui_inkwell.slang`

**Interfaces:**
- Consumes: `kFlagClip` from Task 1 (as `kUiFlagClip` on the shader side).
- Produces: `include/UiVertexOut.slangh` defining `struct VSOutput` (now carrying `nointerpolation float4 clipRect : TEXCOORD8`) and `void UiClipDiscard(VSOutput input)`.

`ui_ink.slang`, `ui_ink_drips.slang`, `ui_void_bg.slang` and `ink_field.slang` declare their own `vertexMain` and are **not** migrated — they own both stages and never pair with the shared vertex shader.

- [ ] **Step 1: Create the shared header**

Create `src/shaders/include/UiVertexOut.slangh`:

```hlsl
#ifndef AETHER_UI_VERTEX_OUT_SLANGH
#define AETHER_UI_VERTEX_OUT_SLANGH

// The interface between the shared UI vertex shader (ui_shapes.slang) and every fragment
// shader that pairs with it - the engine's own, and each project UIMaterial shader.
//
// Project shaders MUST include this rather than copying the struct. A copy that drifts from
// the vertex stage's layout does not fail to compile; it silently reads the wrong varyings.
struct VSOutput
{
    float4 position      : SV_Position;
    float4 color         : TEXCOORD0;
    float2 local         : TEXCOORD1; // [-1,1] for SDF corner/circle tests
    float2 localScale    : TEXCOORD2; // rect pixel size (for corner radius)
    float  cornerRadius  : TEXCOORD3;
    nointerpolation uint type        : TEXCOORD4;
    float2 texUV         : TEXCOORD5; // UV for TexturedRect
    nointerpolation uint textureSlot : TEXCOORD6;
    nointerpolation uint flags       : TEXCOORD7;
    nointerpolation float4 clipRect  : TEXCOORD8; // x,y,w,h px; live only when kUiFlagClip
};

// Mirrors aether::ui::kFlagClip in engine/ui/UiDrawCommand.hpp.
static const uint kUiFlagClip = 4u;

// Discard fragments outside the command's clip rect. SV_Position.xy in the fragment stage is
// already window pixels - the same space UI rects are authored in - so no transform is needed.
// Call this FIRST in every UI fragment entry point, engine or project.
void UiClipDiscard(VSOutput input)
{
    if ((input.flags & kUiFlagClip) != 0u)
    {
        const float2 p = input.position.xy;
        if (p.x < input.clipRect.x || p.y < input.clipRect.y ||
            p.x > input.clipRect.x + input.clipRect.z ||
            p.y > input.clipRect.y + input.clipRect.w)
        {
            discard;
        }
    }
}

#endif
```

- [ ] **Step 2: Use it from ui_shapes.slang**

In `src/shaders/ui_shapes.slang`, add below the existing includes:

```hlsl
#include "include/UiVertexOut.slangh"
```

Delete the local `struct VSOutput { ... };` block (lines 28-39) entirely.

In `vertexMain`, immediately before `return output;`, add:

```hlsl
    output.clipRect    = cmd.clipRect;
```

At the top of `fragmentMain`, as the first statement:

```hlsl
    UiClipDiscard(input);
```

- [ ] **Step 3: Migrate the four project material shaders**

In each of `projects/INKBOUND/assets/shaders/ui_glitch_text.slang`, `ui_dialogue_text.slang`, `ui_ink_ui.slang` and `ui_inkwell.slang`:

1. Add near the top, after the bindless declarations:

```hlsl
// Shared with the engine's UI vertex stage. Do not hand-copy VSOutput - include it.
#include "include/UiVertexOut.slangh"
```

2. Delete that file's local `struct VSOutput { ... };` block.
3. Make `UiClipDiscard(input);` the first statement of its `fragmentMain`.

Project shaders can include engine shader headers; the include path resolves the same way it does for `ui_shapes.slang`.

- [ ] **Step 4: Compile every shader**

```bash
cmake --build build/ninja-clang --target CompileShaders EngineAssetsPak
```

Expected: success. A project shader that still declares its own `VSOutput` errors with a redefinition — that is the new contract working.

- [ ] **Step 5: Commit**

```bash
git add src/shaders/include/UiVertexOut.slangh src/shaders/ui_shapes.slang projects/INKBOUND/assets/shaders
git commit -m "Share the UI VSOutput struct through a header

- Move VSOutput and the clip test into include/UiVertexOut.slangh
- Migrate INKBOUND's UI material shaders off their hand-copied structs"
```

---

### Task 3: Clip stack in the draw builder, and UIMask

**Files:**
- Modify: `src/engine/ui/UiComponents.hpp`
- Modify: `src/engine/ui/UiDrawBuilder.cpp:176-266` (`Walk`) and `:268-298` (`BuildDrawCommands`)
- Modify: `src/app/scene/reflection/MoreComponents.reflect.cpp`
- Test: `tests/ui/UiDrawBuilderTests.cpp`

**Interfaces:**
- Consumes: `kFlagClip`, `UiDrawCommand::clipRect` (Task 1).
- Produces: `struct aether::ui::UIMask { bool enabled = true; float padding = 0.f; };`. Commands emitted under a mask carry `kFlagClip` and the intersected rect. A command that already carries a clip has it **intersected**, not overwritten — Task 9 relies on this so a textbox's own inner clip composes with an ancestor mask.

- [ ] **Step 1: Write the failing tests**

Append to `tests/ui/UiDrawBuilderTests.cpp`:

```cpp
TEST_CASE("UIMask clips its subtree to its own rect")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity mask = w.Create();
	auto& mr = w.Emplace<ui::UIRect>(mask);
	mr.resolvedRect = {100, 100, 200, 50};
	w.Emplace<ui::UIMask>(mask);
	w.Emplace<HierarchyComponent>(mask);
	ecs::SetParent(w, mask, canvas);

	Entity child = w.Create();
	auto& chr = w.Emplace<ui::UIRect>(child);
	chr.resolvedRect = {0, 0, 1000, 800}; // deliberately overflows the mask
	w.Emplace<ui::UIImage>(child);
	w.Emplace<HierarchyComponent>(child);
	ecs::SetParent(w, child, mask);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 1);
	CHECK((cmds[0].flags & ui::kFlagClip) != 0u);
	CHECK(cmds[0].clipRect.x == doctest::Approx(100));
	CHECK(cmds[0].clipRect.y == doctest::Approx(100));
	CHECK(cmds[0].clipRect.z == doctest::Approx(200));
	CHECK(cmds[0].clipRect.w == doctest::Approx(50));
}

TEST_CASE("Nested UIMasks intersect, and padding shrinks the clip")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity outer = w.Create();
	auto& our = w.Emplace<ui::UIRect>(outer);
	our.resolvedRect = {0, 0, 300, 300};
	w.Emplace<ui::UIMask>(outer);
	w.Emplace<HierarchyComponent>(outer);
	ecs::SetParent(w, outer, canvas);

	Entity inner = w.Create();
	auto& inr = w.Emplace<ui::UIRect>(inner);
	inr.resolvedRect = {100, 100, 400, 400}; // overhangs `outer` to the right and bottom
	auto& innerMask = w.Emplace<ui::UIMask>(inner);
	innerMask.padding = 10.f;
	w.Emplace<ui::UIImage>(inner);
	w.Emplace<HierarchyComponent>(inner);
	ecs::SetParent(w, inner, outer);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 1);
	CHECK((cmds[0].flags & ui::kFlagClip) != 0u);
	CHECK(cmds[0].clipRect.x == doctest::Approx(110)); // 100 + 10 padding
	CHECK(cmds[0].clipRect.y == doctest::Approx(110));
	CHECK(cmds[0].clipRect.z == doctest::Approx(190)); // clipped by outer's right edge at 300
	CHECK(cmds[0].clipRect.w == doctest::Approx(190));
}

TEST_CASE("A disabled UIMask does not clip")
{
	World w;

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity mask = w.Create();
	auto& mr = w.Emplace<ui::UIRect>(mask);
	mr.resolvedRect = {100, 100, 200, 50};
	auto& m = w.Emplace<ui::UIMask>(mask);
	m.enabled = false;
	w.Emplace<ui::UIImage>(mask);
	w.Emplace<HierarchyComponent>(mask);
	ecs::SetParent(w, mask, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	ui::BuildDrawCommands(w, cmds);

	REQUIRE(cmds.size() == 1);
	CHECK((cmds[0].flags & ui::kFlagClip) == 0u);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `UIMask` is not a member of `aether::ui`.

- [ ] **Step 3: Add the component**

In `src/engine/ui/UiComponents.hpp`, after `UIProgressBar`:

```cpp
	// Clips this element's own draws and its whole subtree to its UIRect (Unity's RectMask2D).
	// Nested masks intersect. The clip is a screen-space pixel rect tested per fragment, so it
	// costs nothing extra in the batch - masked and unmasked commands share one draw.
	struct UIMask
	{
		bool enabled = true;
		float padding = 0.f; // shrink the clip inwards on every side
	};
```

- [ ] **Step 4: Thread the clip through the builder**

In `src/engine/ui/UiDrawBuilder.cpp`, add above `Walk`:

```cpp
	// The active clip as Walk descends. Rect is (x, y, w, h) px; `active` false = unclipped.
	struct ClipState
	{
		glm::vec4 rect{0.f};
		bool active = false;
	};

	static glm::vec4 IntersectRects(const glm::vec4& a, const glm::vec4& b)
	{
		const float x0 = std::max(a.x, b.x);
		const float y0 = std::max(a.y, b.y);
		const float x1 = std::min(a.x + a.z, b.x + b.z);
		const float y1 = std::min(a.y + a.w, b.y + b.w);
		return {x0, y0, std::max(x1 - x0, 0.f), std::max(y1 - y0, 0.f)};
	}
```

Change `Walk`'s signature to take the clip by value (each branch of the tree gets its own):

```cpp
	static void Walk(World& world, Entity entity, FontRegistry* fonts, TextureRegistry* textures, int& layer, std::vector<UiDrawCommand>& out, std::vector<UiMaterialDraw>& materials, ClipState clip)
```

Immediately after the `DisabledComponent` early-out, before `const std::size_t emitBegin = out.size();`:

```cpp
		// A mask narrows the clip for this element's own draws AND everything below it.
		if (const auto* mask = world.TryGet<UIMask>(entity); mask != nullptr && mask->enabled)
		{
			if (const auto* maskRect = world.TryGet<UIRect>(entity))
			{
				const glm::vec4 r = maskRect->resolvedRect;
				const float p = mask->padding;
				const glm::vec4 padded{r.x + p, r.y + p, std::max(r.z - 2.f * p, 0.f), std::max(r.w - 2.f * p, 0.f)};
				clip.rect = clip.active ? IntersectRects(clip.rect, padded) : padded;
				clip.active = true;
			}
		}
```

In the stamping loop that currently applies `shaderId`, add the clip alongside it. Replace the loop body with:

```cpp
		for (std::size_t i = emitBegin; i < emitEnd; ++i)
		{
			// An element may have clipped its own sub-shapes already (a text box clips its text
			// to its padded inner rect). Compose rather than overwrite, so a self-clip nested in
			// an ancestor mask ends up with the intersection of both.
			if (clip.active)
			{
				if ((out[i].flags & kFlagClip) != 0u)
				{
					out[i].clipRect = IntersectRects(out[i].clipRect, clip.rect);
				}
				else
				{
					out[i].clipRect = clip.rect;
					out[i].flags |= kFlagClip;
				}
			}
			if ((out[i].flags & kFlagNoMaterial) != 0u)
			{
				out[i].flags &= ~kFlagNoMaterial; // consumed; never uploaded
				continue;                         // this sub-shape stays on the default pipeline
			}
			if (shaderId != 0u)
			{
				out[i].flags = UiFlagsWithShaderId(out[i].flags, shaderId);
			}
		}
```

Pass the clip down to children:

```cpp
				Walk(world, child, fonts, textures, layer, out, materials, clip);
```

And seed it in `BuildDrawCommands`:

```cpp
			Walk(world, canvas, fonts, textures, layer, out, materials, ClipState{});
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*UIMask*"
```

Expected: all three PASS.

- [ ] **Step 6: Register UIMask for authoring**

In `src/app/scene/reflection/MoreComponents.reflect.cpp`, add to the alias block near the top:

```cpp
using UiMaskComponent = aether::ui::UIMask;
```

And after the `UiProgressBarComponent` block:

```cpp
AE_COMPONENT(UiMaskComponent, "UI Mask", "UI", ICON_FA_CROP)
AE_FIELD_N("enabled", enabled, Bool)
AE_FIELD_N("padding", padding, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, false); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
```

- [ ] **Step 7: Run the whole suite**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass, including the serializer round-trip tests that walk the component registry.

- [ ] **Step 8: Commit**

```bash
git add src/engine/ui/UiComponents.hpp src/engine/ui/UiDrawBuilder.cpp src/app/scene/reflection/MoreComponents.reflect.cpp tests/ui/UiDrawBuilderTests.cpp
git commit -m "Clip UI draws to a mask rect

- Carry a clip rect down the draw-builder walk, intersecting on nested masks
- Add a UIMask component that clips its subtree"
```

---

### Task 4: Synthetic typed chars and clipboard on Input

**Files:**
- Modify: `src/engine/platform/Input.hpp:190` (near `GetTypedChars`), `:225` (near `ClearSyntheticKeys`), `:302` (members)
- Modify: `src/engine/platform/Input.cpp:118` (in `Update`), `:230` (near `GetTypedChars`)
- Modify: `src/app/editor/ControlMethods.cpp:1414-1425`
- Test: `tests/ui/UiTextEditTests.cpp` (created here, extended by Tasks 5-6)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `void Input::SetSyntheticChars(std::string_view chars)` — appends to next frame's typed chars.
  - `void Input::ClearSyntheticChars()`
  - `[[nodiscard]] std::string Input::GetClipboardText() const`
  - `void Input::SetClipboardText(std::string_view text)`
  - MCP `engine.send_input` accepts `{"text": "..."}`.

`Input::Update()` calls `glfwGetKey(m_window, ...)` unconditionally, so a windowless `Input` must never have `Update()` called on it. The clipboard accessors are the only members safe to use windowless — that is deliberate, and it is the test seam.

- [ ] **Step 1: Write the failing test**

Create `tests/ui/UiTextEditTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <string>

#include "platform/Input.hpp"

using namespace aether;

TEST_CASE("Clipboard falls back to an internal string with no window")
{
	Input input; // no Init(): windowless, as in unit tests

	CHECK(input.GetClipboardText().empty());

	input.SetClipboardText("192.168.0.1");
	CHECK(input.GetClipboardText() == "192.168.0.1");

	input.SetClipboardText("");
	CHECK(input.GetClipboardText().empty());
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `GetClipboardText` is not a member of `aether::Input`.

- [ ] **Step 3: Declare the new API**

In `src/engine/platform/Input.hpp`, after the `GetTypedChars()` declaration:

```cpp
		// OS clipboard. With no window (unit tests) both fall back to an internal string, so
		// cut/copy/paste logic is exercisable headless.
		[[nodiscard]] std::string GetClipboardText() const;
		void SetClipboardText(std::string_view text);
```

After `ClearSyntheticKeys()`:

```cpp
		// Synthetic TEXT injection, the character-level counterpart to SetSyntheticKey: the
		// string is appended to the next frame's GetTypedChars(), so a headless test types
		// through exactly the path a real keyboard does. Consumed and cleared each Update().
		void SetSyntheticChars(std::string_view chars)
		{
			m_pendingChars.append(chars);
		}

		void ClearSyntheticChars()
		{
			m_pendingChars.clear();
		}
```

Add to the member block beside `m_typedChars`:

```cpp
		std::string m_clipboardFallback;
```

Add `#include <string_view>` to the header's includes if not already present.

- [ ] **Step 4: Implement the clipboard**

In `src/engine/platform/Input.cpp`, after `GetTypedChars()`:

```cpp
	std::string Input::GetClipboardText() const
	{
		if (m_window != nullptr)
		{
			const char* text = glfwGetClipboardString(m_window);
			return text != nullptr ? std::string{text} : std::string{};
		}
		return m_clipboardFallback;
	}

	void Input::SetClipboardText(std::string_view text)
	{
		const std::string owned{text};
		if (m_window != nullptr)
		{
			glfwSetClipboardString(m_window, owned.c_str());
			return;
		}
		m_clipboardFallback = owned;
	}
```

`SetSyntheticChars` already appends into `m_pendingChars`, which `Update()` moves into `m_typedChars` at line 118 — no change needed there.

- [ ] **Step 5: Run the test**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="Clipboard falls back*"
```

Expected: PASS.

- [ ] **Step 6: Expose text injection over MCP**

In `src/app/editor/ControlMethods.cpp`, in the `engine.send_input` registration, extend the description string to end with:

```
 Pass {text:"..."} to inject typed characters (text fields) alongside key state.
```

Add to the `Obj({...})` schema, after the `"clear"` entry:

```cpp
		                {"text", StrProp()},
```

And in the handler, after the `apply("up", false);` line:

```cpp
			        // Typed text goes through the same char queue a real keyboard fills, so a
			        // headless test drives text fields exactly like a player: type, then Enter.
			        if (params.contains("text") && params["text"].is_string())
			        {
				        input->SetSyntheticChars(params["text"].get<std::string>());
			        }
```

Also extend the `clear` branch:

```cpp
			        if (params.value("clear", false))
			        {
				        input->ClearSyntheticKeys();
				        input->ClearSyntheticMouse();
				        input->ClearSyntheticChars();
			        }
```

- [ ] **Step 7: Build the editor**

```bash
cmake --build build/ninja-clang --target Editor
```

Expected: success. `tools/mcp/manifest.json` is a cache the MCP server re-merges from the live engine — do not hand-edit it; restart the MCP server to see the new field.

- [ ] **Step 8: Commit**

```bash
git add src/engine/platform/Input.hpp src/engine/platform/Input.cpp src/app/editor/ControlMethods.cpp tests/ui/UiTextEditTests.cpp
git commit -m "Add clipboard access and synthetic typed text to Input

- Read/write the OS clipboard, with an internal fallback when there is no window
- Inject typed characters through the real char queue, exposed as send_input {text}"
```

---

### Task 5: Text editing core — state, filtering, insert and delete

**Files:**
- Create: `src/engine/ui/UiTextEdit.hpp`, `src/engine/ui/UiTextEdit.cpp`
- Test: `tests/ui/UiTextEditTests.cpp`

**Interfaces:**
- Consumes: nothing (pure).
- Produces, all in `namespace aether::ui`:
  - `enum class TextContentType : std::uint8_t { Any, Integer, Decimal, Alphanumeric, IpAddress }`
  - `struct TextEditState { std::string text; int caret = 0; int selectionAnchor = 0; float scrollX = 0.f; }`
  - `struct TextEditLimits { TextContentType contentType = TextContentType::Any; std::string allowedChars; int maxLength = 0; }`
  - `int SelectionBegin(const TextEditState&)`, `int SelectionEnd(const TextEditState&)`, `bool HasSelection(const TextEditState&)`, `void ClearSelection(TextEditState&)`, `void SelectAll(TextEditState&)`, `std::string SelectedText(const TextEditState&)`
  - `std::string FilterInsert(const TextEditState&, const TextEditLimits&, std::string_view chars)`
  - `bool InsertText(TextEditState&, const TextEditLimits&, std::string_view chars)`
  - `bool DeleteSelection(TextEditState&)`
  - `std::string DisplayText(std::string_view text, bool password)`

All mutators return `true` when `text` changed, so the caller sets `changed` in one place.

- [ ] **Step 1: Write the failing tests**

Append to `tests/ui/UiTextEditTests.cpp` (and add `#include "ui/UiTextEdit.hpp"` at the top):

```cpp
TEST_CASE("FilterInsert drops non-printable and non-ASCII input")
{
	ui::TextEditState s;
	const ui::TextEditLimits limits;

	CHECK(ui::FilterInsert(s, limits, "abc") == "abc");
	CHECK(ui::FilterInsert(s, limits, "a\nb\tc") == "abc");   // control chars dropped
	CHECK(ui::FilterInsert(s, limits, "caf\xC3\xA9") == "caf"); // UTF-8 continuation bytes dropped
}

TEST_CASE("FilterInsert honours the content type")
{
	ui::TextEditState s;

	ui::TextEditLimits ints;
	ints.contentType = ui::TextContentType::Integer;
	CHECK(ui::FilterInsert(s, ints, "12a3") == "123");

	ui::TextEditLimits dec;
	dec.contentType = ui::TextContentType::Decimal;
	CHECK(ui::FilterInsert(s, dec, "1.5x") == "1.5");

	ui::TextEditLimits alnum;
	alnum.contentType = ui::TextContentType::Alphanumeric;
	CHECK(ui::FilterInsert(s, alnum, "ab-12_") == "ab12");

	ui::TextEditLimits ip;
	ip.contentType = ui::TextContentType::IpAddress;
	CHECK(ui::FilterInsert(s, ip, "192.168.0.1:7777") == "192.168.0.1:7777");
	CHECK(ui::FilterInsert(s, ip, "host name") == "hostname");
}

TEST_CASE("FilterInsert intersects contentType with allowedChars")
{
	ui::TextEditState s;
	ui::TextEditLimits limits;
	limits.contentType = ui::TextContentType::Integer;
	limits.allowedChars = "0123";

	CHECK(ui::FilterInsert(s, limits, "0123456789") == "0123");
	CHECK(ui::FilterInsert(s, limits, "1a2") == "12");
}

TEST_CASE("FilterInsert respects maxLength against the current text")
{
	ui::TextEditState s;
	s.text = "abc";
	s.caret = 3;
	s.selectionAnchor = 3;

	ui::TextEditLimits limits;
	limits.maxLength = 5;

	CHECK(ui::FilterInsert(s, limits, "de") == "de");
	CHECK(ui::FilterInsert(s, limits, "defg") == "de"); // truncated at the limit

	// A selection is about to be replaced, so its length is budget.
	s.selectionAnchor = 0;
	CHECK(ui::FilterInsert(s, limits, "defg") == "defg");
}

TEST_CASE("InsertText inserts at the caret and advances it")
{
	ui::TextEditState s;
	const ui::TextEditLimits limits;

	CHECK(ui::InsertText(s, limits, "hello"));
	CHECK(s.text == "hello");
	CHECK(s.caret == 5);
	CHECK(s.selectionAnchor == 5);

	s.caret = 0;
	s.selectionAnchor = 0;
	CHECK(ui::InsertText(s, limits, ">"));
	CHECK(s.text == ">hello");
	CHECK(s.caret == 1);

	CHECK_FALSE(ui::InsertText(s, limits, "")); // nothing survived filtering
	CHECK(s.text == ">hello");
}

TEST_CASE("InsertText replaces the selection")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 6;
	s.caret = 11;

	const ui::TextEditLimits limits;
	CHECK(ui::InsertText(s, limits, "there"));
	CHECK(s.text == "hello there");
	CHECK(s.caret == 11);
	CHECK_FALSE(ui::HasSelection(s));
}

TEST_CASE("Selection helpers order the anchor and caret")
{
	ui::TextEditState s;
	s.text = "abcdef";
	s.selectionAnchor = 4;
	s.caret = 1;

	CHECK(ui::HasSelection(s));
	CHECK(ui::SelectionBegin(s) == 1);
	CHECK(ui::SelectionEnd(s) == 4);
	CHECK(ui::SelectedText(s) == "bcd");

	ui::ClearSelection(s);
	CHECK_FALSE(ui::HasSelection(s));
	CHECK(s.selectionAnchor == 1);

	ui::SelectAll(s);
	CHECK(ui::SelectionBegin(s) == 0);
	CHECK(ui::SelectionEnd(s) == 6);
	CHECK(s.caret == 6);
}

TEST_CASE("DeleteSelection removes the range and collapses the caret")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 5;
	s.caret = 11;

	CHECK(ui::DeleteSelection(s));
	CHECK(s.text == "hello");
	CHECK(s.caret == 5);
	CHECK_FALSE(ui::HasSelection(s));

	CHECK_FALSE(ui::DeleteSelection(s)); // nothing selected
}

TEST_CASE("DisplayText masks a password")
{
	CHECK(ui::DisplayText("secret", false) == "secret");
	CHECK(ui::DisplayText("secret", true) == "******");
	CHECK(ui::DisplayText("", true).empty());
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `ui/UiTextEdit.hpp` not found.

- [ ] **Step 3: Write the header**

Create `src/engine/ui/UiTextEdit.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aether::ui
{
	// Pure single-line text editing. No World, no Input, no GLFW - so every rule here is
	// unit-testable directly, the same way SliderQuantize is. UiTextBoxSystem is the only
	// caller; it just marshals ECS + Input into these.
	//
	// ASCII ONLY: ShapeText treats each char byte as a codepoint and the baked atlases are
	// ASCII, so multibyte input would render as mojibake rather than simply fail. Insertion
	// filters to printable ASCII 0x20-0x7E, which makes every index below a byte index that
	// is also a character index.

	enum class TextContentType : std::uint8_t
	{
		Any,
		Integer,
		Decimal,
		Alphanumeric,
		IpAddress // digits, dots and colons: "192.168.0.1:7777"
	};

	struct TextEditState
	{
		std::string text;
		int caret = 0;           // insertion point, 0..text.size()
		int selectionAnchor = 0; // the fixed end of the selection; == caret means no selection
		float scrollX = 0.f;     // px of text scrolled off the left edge
	};

	struct TextEditLimits
	{
		TextContentType contentType = TextContentType::Any;
		std::string allowedChars; // non-empty = whitelist, ANDed with contentType
		int maxLength = 0;        // 0 = unlimited
	};

	[[nodiscard]] int SelectionBegin(const TextEditState& s);
	[[nodiscard]] int SelectionEnd(const TextEditState& s);
	[[nodiscard]] bool HasSelection(const TextEditState& s);
	[[nodiscard]] std::string SelectedText(const TextEditState& s);
	void ClearSelection(TextEditState& s);
	void SelectAll(TextEditState& s);

	// The subset of `chars` acceptable right now: printable ASCII, passing contentType and
	// allowedChars, truncated to whatever maxLength budget remains once the current selection
	// (which the insert would replace) is accounted for.
	[[nodiscard]] std::string FilterInsert(const TextEditState& s, const TextEditLimits& limits, std::string_view chars);

	// Each returns true when `text` actually changed, so the caller raises `changed` once.
	bool InsertText(TextEditState& s, const TextEditLimits& limits, std::string_view chars);
	bool DeleteSelection(TextEditState& s);

	// `text` with every character replaced by '*' when password is set.
	[[nodiscard]] std::string DisplayText(std::string_view text, bool password);
} // namespace aether::ui
```

- [ ] **Step 4: Write the implementation**

Create `src/engine/ui/UiTextEdit.cpp`:

```cpp
#include "ui/UiTextEdit.hpp"

#include <algorithm>

namespace aether::ui
{
	namespace
	{
		bool IsPrintableAscii(char c)
		{
			const auto u = static_cast<unsigned char>(c);
			return u >= 0x20 && u <= 0x7E;
		}

		bool PassesContentType(char c, TextContentType type)
		{
			const bool digit = c >= '0' && c <= '9';
			const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			switch (type)
			{
			case TextContentType::Integer:
				return digit || c == '-';
			case TextContentType::Decimal:
				return digit || c == '-' || c == '.';
			case TextContentType::Alphanumeric:
				return digit || alpha;
			case TextContentType::IpAddress:
				return digit || c == '.' || c == ':';
			case TextContentType::Any:
				break;
			}
			return true;
		}

		int ClampIndex(int i, std::size_t size)
		{
			return std::clamp(i, 0, static_cast<int>(size));
		}
	} // namespace

	int SelectionBegin(const TextEditState& s)
	{
		return std::min(ClampIndex(s.caret, s.text.size()), ClampIndex(s.selectionAnchor, s.text.size()));
	}

	int SelectionEnd(const TextEditState& s)
	{
		return std::max(ClampIndex(s.caret, s.text.size()), ClampIndex(s.selectionAnchor, s.text.size()));
	}

	bool HasSelection(const TextEditState& s)
	{
		return SelectionBegin(s) != SelectionEnd(s);
	}

	std::string SelectedText(const TextEditState& s)
	{
		const int begin = SelectionBegin(s);
		return s.text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(SelectionEnd(s) - begin));
	}

	void ClearSelection(TextEditState& s)
	{
		s.selectionAnchor = s.caret;
	}

	void SelectAll(TextEditState& s)
	{
		s.selectionAnchor = 0;
		s.caret = static_cast<int>(s.text.size());
	}

	std::string FilterInsert(const TextEditState& s, const TextEditLimits& limits, std::string_view chars)
	{
		std::string out;
		out.reserve(chars.size());
		for (const char c: chars)
		{
			if (!IsPrintableAscii(c) || !PassesContentType(c, limits.contentType))
			{
				continue;
			}
			if (!limits.allowedChars.empty() && limits.allowedChars.find(c) == std::string::npos)
			{
				continue;
			}
			out.push_back(c);
		}

		if (limits.maxLength > 0)
		{
			// The selection is about to be replaced, so its length is budget, not spend.
			const int selected = SelectionEnd(s) - SelectionBegin(s);
			const int remaining = limits.maxLength - (static_cast<int>(s.text.size()) - selected);
			if (remaining <= 0)
			{
				return {};
			}
			if (static_cast<int>(out.size()) > remaining)
			{
				out.resize(static_cast<std::size_t>(remaining));
			}
		}
		return out;
	}

	bool InsertText(TextEditState& s, const TextEditLimits& limits, std::string_view chars)
	{
		const std::string accepted = FilterInsert(s, limits, chars);
		if (accepted.empty())
		{
			return false;
		}

		DeleteSelection(s);
		s.caret = ClampIndex(s.caret, s.text.size());
		s.text.insert(static_cast<std::size_t>(s.caret), accepted);
		s.caret += static_cast<int>(accepted.size());
		ClearSelection(s);
		return true;
	}

	bool DeleteSelection(TextEditState& s)
	{
		if (!HasSelection(s))
		{
			return false;
		}
		const int begin = SelectionBegin(s);
		const int end = SelectionEnd(s);
		s.text.erase(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
		s.caret = begin;
		ClearSelection(s);
		return true;
	}

	std::string DisplayText(std::string_view text, bool password)
	{
		return password ? std::string(text.size(), '*') : std::string{text};
	}
} // namespace aether::ui
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*FilterInsert*,*InsertText*,*Selection*,*DisplayText*"
```

Expected: all PASS. The new `.cpp` triggers a CMake reconfigure on this first build (globbed with `CONFIGURE_DEPENDS`).

- [ ] **Step 6: Commit**

```bash
git add src/engine/ui/UiTextEdit.hpp src/engine/ui/UiTextEdit.cpp tests/ui/UiTextEditTests.cpp
git commit -m "Add the text editing core: filtering, insert and delete"
```

---

### Task 6: Text editing core — caret movement, measurement, hit-testing, scroll

**Files:**
- Modify: `src/engine/ui/UiTextEdit.hpp`, `src/engine/ui/UiTextEdit.cpp`
- Test: `tests/ui/UiTextEditTests.cpp`

**Interfaces:**
- Consumes: `TextEditState`, `TextEditLimits` (Task 5).
- Produces:
  - `enum class CaretMove : std::uint8_t { Left, Right, WordLeft, WordRight, Home, End }`
  - `void MoveCaret(TextEditState&, CaretMove, bool extendSelection)`
  - `int WordBoundary(std::string_view text, int from, int dir)` — `dir` is `-1` or `+1`
  - `bool DeleteBackward(TextEditState&, bool wholeWord)`, `bool DeleteForward(TextEditState&, bool wholeWord)`
  - `float TextWidth(const FontAsset&, std::string_view text, float pixelSize)`
  - `float CaretToPixelX(const FontAsset&, std::string_view text, float pixelSize, int caret)`
  - `int CaretFromPixelX(const FontAsset&, std::string_view text, float pixelSize, float localX)`
  - `void ScrollToCaret(TextEditState&, const FontAsset&, float pixelSize, float innerWidth, bool password)`
  - `void SelectWordAt(TextEditState&, int index)`

Measurement takes `const FontAsset&` — a plain struct with a `glyphs` map, no GPU or GLFW dependency, so tests build one by hand exactly as `UiDrawBuilderTests.cpp` already does with `MakeMonoFont()`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/ui/UiTextEditTests.cpp` (add `#include "ui/FontAsset.hpp"` at the top):

```cpp
// Mirrors MakeMonoFont in UiDrawBuilderTests: bakeSize 48, advance 24 -> at pixelSize 48 every
// glyph is exactly 24 px wide, so expected pixel positions are caret * 24.
static ui::FontAsset MakeTextEditFont()
{
	ui::FontAsset f;
	f.atlasBindlessSlot = 42;
	f.atlasWidth = f.atlasHeight = 128;
	f.ascent = 40;
	f.descent = 10;
	f.lineHeight = 50;
	f.bakeSize = 48;
	for (char c = 0x20; c > 0 && c <= 0x7E; ++c)
	{
		f.glyphs[static_cast<std::uint32_t>(c)] = ui::GlyphMeta{static_cast<std::uint32_t>(c), 0.f, 0.f, 0.1f, 0.1f, 20.f, 30.f, 0.f, 30.f, 24.f};
	}
	return f;
}

TEST_CASE("MoveCaret steps by character and clamps at the ends")
{
	ui::TextEditState s;
	s.text = "abc";
	s.caret = 1;
	s.selectionAnchor = 1;

	ui::MoveCaret(s, ui::CaretMove::Right, false);
	CHECK(s.caret == 2);
	ui::MoveCaret(s, ui::CaretMove::Right, false);
	ui::MoveCaret(s, ui::CaretMove::Right, false);
	CHECK(s.caret == 3); // clamped

	ui::MoveCaret(s, ui::CaretMove::Left, false);
	CHECK(s.caret == 2);
	ui::MoveCaret(s, ui::CaretMove::Home, false);
	CHECK(s.caret == 0);
	ui::MoveCaret(s, ui::CaretMove::Left, false);
	CHECK(s.caret == 0); // clamped
	ui::MoveCaret(s, ui::CaretMove::End, false);
	CHECK(s.caret == 3);
}

TEST_CASE("MoveCaret collapses a selection instead of stepping")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 2;
	s.caret = 7;

	ui::MoveCaret(s, ui::CaretMove::Left, false);
	CHECK(s.caret == 2); // collapses to the selection start, does not step to 6
	CHECK_FALSE(ui::HasSelection(s));

	s.selectionAnchor = 2;
	s.caret = 7;
	ui::MoveCaret(s, ui::CaretMove::Right, false);
	CHECK(s.caret == 7); // collapses to the selection end
	CHECK_FALSE(ui::HasSelection(s));
}

TEST_CASE("MoveCaret with extendSelection keeps the anchor")
{
	ui::TextEditState s;
	s.text = "hello";
	s.caret = 2;
	s.selectionAnchor = 2;

	ui::MoveCaret(s, ui::CaretMove::Right, true);
	CHECK(s.caret == 3);
	CHECK(s.selectionAnchor == 2);
	CHECK(ui::SelectedText(s) == "l");

	ui::MoveCaret(s, ui::CaretMove::End, true);
	CHECK(s.caret == 5);
	CHECK(s.selectionAnchor == 2);
	CHECK(ui::SelectedText(s) == "llo");
}

TEST_CASE("WordBoundary skips runs of word characters and separators")
{
	const std::string_view text = "hello big world";

	CHECK(ui::WordBoundary(text, 15, -1) == 10); // back over "world"
	CHECK(ui::WordBoundary(text, 10, -1) == 6);  // back over " big" -> start of "big"
	CHECK(ui::WordBoundary(text, 0, -1) == 0);   // clamped

	CHECK(ui::WordBoundary(text, 0, 1) == 5);    // forward over "hello"
	CHECK(ui::WordBoundary(text, 5, 1) == 9);    // forward over " big"
	CHECK(ui::WordBoundary(text, 15, 1) == 15);  // clamped
}

TEST_CASE("DeleteBackward and DeleteForward remove one character or one word")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.caret = 11;
	s.selectionAnchor = 11;

	CHECK(ui::DeleteBackward(s, false));
	CHECK(s.text == "hello worl");
	CHECK(s.caret == 10);

	CHECK(ui::DeleteBackward(s, true));
	CHECK(s.text == "hello ");
	CHECK(s.caret == 6);

	s.caret = 0;
	s.selectionAnchor = 0;
	CHECK_FALSE(ui::DeleteBackward(s, false)); // nothing to the left

	CHECK(ui::DeleteForward(s, false));
	CHECK(s.text == "ello ");
	CHECK(s.caret == 0);

	s.text = "abc";
	s.caret = 3;
	s.selectionAnchor = 3;
	CHECK_FALSE(ui::DeleteForward(s, false)); // nothing to the right
}

TEST_CASE("Delete keys remove the selection when there is one")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 0;
	s.caret = 6;

	CHECK(ui::DeleteBackward(s, false));
	CHECK(s.text == "world");
	CHECK(s.caret == 0);
}

TEST_CASE("Text measurement maps carets to pixels and back")
{
	const ui::FontAsset font = MakeTextEditFont();

	CHECK(ui::TextWidth(font, "abc", 48.f) == doctest::Approx(72.f));
	CHECK(ui::TextWidth(font, "", 48.f) == doctest::Approx(0.f));

	CHECK(ui::CaretToPixelX(font, "abc", 48.f, 0) == doctest::Approx(0.f));
	CHECK(ui::CaretToPixelX(font, "abc", 48.f, 2) == doctest::Approx(48.f));
	CHECK(ui::CaretToPixelX(font, "abc", 48.f, 3) == doctest::Approx(72.f));

	// Hit-testing snaps to the nearest gap between characters.
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, -5.f) == 0);
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, 10.f) == 0);
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, 14.f) == 1);
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, 500.f) == 3);
}

TEST_CASE("ScrollToCaret keeps the caret inside the visible window")
{
	const ui::FontAsset font = MakeTextEditFont();

	ui::TextEditState s;
	s.text = "abcdefghij"; // 240 px at 24 px/char
	s.caret = 10;
	s.selectionAnchor = 10;

	ui::ScrollToCaret(s, font, 48.f, 100.f, false);
	CHECK(s.scrollX == doctest::Approx(140.f)); // caret at 240 sits on the right edge

	s.caret = 0;
	ui::ScrollToCaret(s, font, 48.f, 100.f, false);
	CHECK(s.scrollX == doctest::Approx(0.f)); // scrolled back to reveal the start

	// Short text never scrolls, whatever the caret did before.
	s.text = "ab";
	s.caret = 2;
	s.scrollX = 90.f;
	ui::ScrollToCaret(s, font, 48.f, 100.f, false);
	CHECK(s.scrollX == doctest::Approx(0.f));
}

TEST_CASE("SelectWordAt selects the word under an index")
{
	ui::TextEditState s;
	s.text = "hello big world";

	ui::SelectWordAt(s, 7);
	CHECK(ui::SelectedText(s) == "big");
	CHECK(s.caret == 9);
	CHECK(s.selectionAnchor == 6);

	ui::SelectWordAt(s, 0);
	CHECK(ui::SelectedText(s) == "hello");
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile errors — `MoveCaret`, `WordBoundary`, `TextWidth` and friends are undeclared.

- [ ] **Step 3: Extend the header**

In `src/engine/ui/UiTextEdit.hpp`, add `#include "ui/FontAsset.hpp"` to the includes, and append inside the namespace:

```cpp
	enum class CaretMove : std::uint8_t
	{
		Left,
		Right,
		WordLeft,
		WordRight,
		Home,
		End
	};

	// A plain Left/Right with a live selection collapses to that end rather than stepping -
	// what every text field does, and what stops the caret jumping over a character the user
	// just selected.
	void MoveCaret(TextEditState& s, CaretMove move, bool extendSelection);

	// The index one word away from `from` in direction `dir` (-1 back, +1 forward): skip any
	// run of separators, then the run of word characters.
	[[nodiscard]] int WordBoundary(std::string_view text, int from, int dir);

	bool DeleteBackward(TextEditState& s, bool wholeWord);
	bool DeleteForward(TextEditState& s, bool wholeWord);

	void SelectWordAt(TextEditState& s, int index);

	// Measurement. Advance-only: single line, no kerning, matching what ShapeText does.
	[[nodiscard]] float TextWidth(const FontAsset& font, std::string_view text, float pixelSize);
	[[nodiscard]] float CaretToPixelX(const FontAsset& font, std::string_view text, float pixelSize, int caret);
	// `localX` is relative to the text origin (i.e. mouse x - inner.x + scrollX).
	[[nodiscard]] int CaretFromPixelX(const FontAsset& font, std::string_view text, float pixelSize, float localX);

	// Adjust scrollX so the caret is inside [0, innerWidth]. Text that fits never scrolls.
	void ScrollToCaret(TextEditState& s, const FontAsset& font, float pixelSize, float innerWidth, bool password);
```

- [ ] **Step 4: Extend the implementation**

Append to `src/engine/ui/UiTextEdit.cpp`, inside the anonymous namespace:

```cpp
		bool IsWordChar(char c)
		{
			const bool digit = c >= '0' && c <= '9';
			const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			return digit || alpha || c == '_';
		}

		float GlyphAdvance(const FontAsset& font, char c, float scale)
		{
			const GlyphMeta* glyph = font.Find(static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
			return glyph != nullptr ? glyph->advance * scale : 0.f;
		}
```

And in the namespace proper:

```cpp
	int WordBoundary(std::string_view text, int from, int dir)
	{
		const int size = static_cast<int>(text.size());
		int i = std::clamp(from, 0, size);
		if (dir < 0)
		{
			while (i > 0 && !IsWordChar(text[static_cast<std::size_t>(i - 1)]))
			{
				--i;
			}
			while (i > 0 && IsWordChar(text[static_cast<std::size_t>(i - 1)]))
			{
				--i;
			}
			return i;
		}
		while (i < size && !IsWordChar(text[static_cast<std::size_t>(i)]))
		{
			++i;
		}
		while (i < size && IsWordChar(text[static_cast<std::size_t>(i)]))
		{
			++i;
		}
		return i;
	}

	void MoveCaret(TextEditState& s, CaretMove move, bool extendSelection)
	{
		const int size = static_cast<int>(s.text.size());
		const bool hadSelection = HasSelection(s);
		int caret = ClampIndex(s.caret, s.text.size());

		switch (move)
		{
		case CaretMove::Left:
			// Collapse rather than step, so an arrow after a selection lands on its edge.
			caret = (hadSelection && !extendSelection) ? SelectionBegin(s) : std::max(caret - 1, 0);
			break;
		case CaretMove::Right:
			caret = (hadSelection && !extendSelection) ? SelectionEnd(s) : std::min(caret + 1, size);
			break;
		case CaretMove::WordLeft:
			caret = WordBoundary(s.text, caret, -1);
			break;
		case CaretMove::WordRight:
			caret = WordBoundary(s.text, caret, 1);
			break;
		case CaretMove::Home:
			caret = 0;
			break;
		case CaretMove::End:
			caret = size;
			break;
		}

		s.caret = caret;
		if (!extendSelection)
		{
			ClearSelection(s);
		}
	}

	bool DeleteBackward(TextEditState& s, bool wholeWord)
	{
		if (DeleteSelection(s))
		{
			return true;
		}
		s.caret = ClampIndex(s.caret, s.text.size());
		if (s.caret == 0)
		{
			return false;
		}
		const int begin = wholeWord ? WordBoundary(s.text, s.caret, -1) : s.caret - 1;
		s.text.erase(static_cast<std::size_t>(begin), static_cast<std::size_t>(s.caret - begin));
		s.caret = begin;
		ClearSelection(s);
		return true;
	}

	bool DeleteForward(TextEditState& s, bool wholeWord)
	{
		if (DeleteSelection(s))
		{
			return true;
		}
		s.caret = ClampIndex(s.caret, s.text.size());
		if (s.caret >= static_cast<int>(s.text.size()))
		{
			return false;
		}
		const int end = wholeWord ? WordBoundary(s.text, s.caret, 1) : s.caret + 1;
		s.text.erase(static_cast<std::size_t>(s.caret), static_cast<std::size_t>(end - s.caret));
		ClearSelection(s);
		return true;
	}

	void SelectWordAt(TextEditState& s, int index)
	{
		const int i = ClampIndex(index, s.text.size());
		// Step right first so an index sitting on a word start still selects that word.
		const int end = WordBoundary(s.text, i, 1);
		s.selectionAnchor = WordBoundary(s.text, end, -1);
		s.caret = end;
	}

	float TextWidth(const FontAsset& font, std::string_view text, float pixelSize)
	{
		if (font.bakeSize <= 0.f)
		{
			return 0.f;
		}
		const float scale = pixelSize / font.bakeSize;
		float width = 0.f;
		for (const char c: text)
		{
			width += GlyphAdvance(font, c, scale);
		}
		return width;
	}

	float CaretToPixelX(const FontAsset& font, std::string_view text, float pixelSize, int caret)
	{
		const int clamped = std::clamp(caret, 0, static_cast<int>(text.size()));
		return TextWidth(font, text.substr(0, static_cast<std::size_t>(clamped)), pixelSize);
	}

	int CaretFromPixelX(const FontAsset& font, std::string_view text, float pixelSize, float localX)
	{
		if (font.bakeSize <= 0.f || text.empty())
		{
			return 0;
		}
		const float scale = pixelSize / font.bakeSize;
		float x = 0.f;
		for (std::size_t i = 0; i < text.size(); ++i)
		{
			const float advance = GlyphAdvance(font, text[i], scale);
			// Past the halfway point of a glyph the caret belongs after it.
			if (localX < x + advance * 0.5f)
			{
				return static_cast<int>(i);
			}
			x += advance;
		}
		return static_cast<int>(text.size());
	}

	void ScrollToCaret(TextEditState& s, const FontAsset& font, float pixelSize, float innerWidth, bool password)
	{
		const std::string display = DisplayText(s.text, password);
		const float total = TextWidth(font, display, pixelSize);
		if (total <= innerWidth)
		{
			s.scrollX = 0.f; // it all fits; never leave the view scrolled
			return;
		}

		const float caretX = CaretToPixelX(font, display, pixelSize, s.caret);
		s.scrollX = std::clamp(s.scrollX, caretX - innerWidth, caretX);
		// Never scroll past the end of the text, and never before its start.
		s.scrollX = std::clamp(s.scrollX, 0.f, total - innerWidth);
	}
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*Caret*,*WordBoundary*,*Delete*,*measurement*,*Scroll*,*SelectWordAt*"
```

Expected: all PASS.

- [ ] **Step 6: Run the whole suite**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine/ui/UiTextEdit.hpp src/engine/ui/UiTextEdit.cpp tests/ui/UiTextEditTests.cpp
git commit -m "Add caret movement, measurement and scrolling to the text editing core"
```

---

### Task 7: Components and the navigation capture contract

**Files:**
- Modify: `src/engine/ui/UiComponents.hpp`
- Modify: `src/engine/ui/UiNavigationSystem.cpp:139-190`
- Test: `tests/ui/UiNavigationSystemTests.cpp` (create if absent)

**Interfaces:**
- Consumes: `TextContentType` (Task 5).
- Produces:
  - `struct aether::ui::UIKeyboardCapture {}` — marker. While the focused entity has it, `UiNavigationSystem` does not consume Left/Right/Up/Down/Enter/Space.
  - `struct aether::ui::UITextBox { ... }` — full field list below; Task 8 drives it, Task 9 draws it.
  - Tab (and Shift+Tab) move focus in reading order, whether or not focus is captured.

Mouse hover, click focusing and click activation stay unconditional — that is how a user leaves a field.

- [ ] **Step 1: Write the failing tests**

Create `tests/ui/UiNavigationSystemTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "platform/Input.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiNavigationSystem.hpp"

using namespace aether;

namespace
{
	Entity MakeSelectable(World& w, glm::vec4 rect)
	{
		const Entity e = w.Create();
		auto& r = w.Emplace<ui::UIRect>(e);
		r.resolvedRect = rect;
		w.Emplace<ui::UISelectable>(e);
		return e;
	}

	// World exposes TryGet, not Get - these entities always have the component.
	ui::UISelectable& Sel(World& w, Entity e)
	{
		return *w.TryGet<ui::UISelectable>(e);
	}
} // namespace

TEST_CASE("A captured focus keeps navigation keys away from the nav system")
{
	World w;
	Input input;

	const Entity top = MakeSelectable(w, {0, 0, 100, 20});
	const Entity bottom = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, top).focused = true;
	w.Emplace<ui::UIKeyboardCapture>(top);

	input.SetSyntheticKey(static_cast<int>(Key::Right), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, top).focused);
	CHECK_FALSE(Sel(w, bottom).focused);
	CHECK_FALSE(Sel(w, top).activated);
}

TEST_CASE("A captured focus is not activated by Enter")
{
	World w;
	Input input;

	const Entity field = MakeSelectable(w, {0, 0, 100, 20});
	Sel(w, field).focused = true;
	w.Emplace<ui::UIKeyboardCapture>(field);

	input.SetSyntheticKey(static_cast<int>(Key::Enter), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, field).activated);
}

TEST_CASE("Tab advances focus in reading order even while captured")
{
	World w;
	Input input;

	const Entity first = MakeSelectable(w, {0, 0, 100, 20});
	const Entity second = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, first).focused = true;
	w.Emplace<ui::UIKeyboardCapture>(first);

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK_FALSE(Sel(w, first).focused);
	CHECK(Sel(w, second).focused);
}

TEST_CASE("Shift+Tab moves focus backwards and wraps")
{
	World w;
	Input input;

	const Entity first = MakeSelectable(w, {0, 0, 100, 20});
	const Entity second = MakeSelectable(w, {0, 100, 100, 20});

	Sel(w, first).focused = true;

	input.SetSyntheticKey(static_cast<int>(Key::Tab), true);
	input.SetSyntheticKey(static_cast<int>(Key::LeftShift), true);
	ui::UiNavigationSystem::Update(w, input);

	CHECK(Sel(w, second).focused); // wrapped from first to last
}
```

`Input::SetSyntheticKey` sets state that `Update()` would OR in, but these tests never call `Input::Update()` (it would dereference a null GLFW window). Check how the existing suite drives `Input`; if no windowless read path exists, add one small accessor rather than initialising GLFW:

In `src/engine/platform/Input.hpp`, inside `SetSyntheticKey`, also seed the current-state array so a windowless caller sees the key immediately:

```cpp
		void SetSyntheticKey(int key, bool down)
		{
			if (key >= 0 && key < kMaxKeys)
			{
				m_syntheticKeys[key] = down;
				// Seed the live state too: Update() re-derives this from GLFW every frame, so
				// this only matters where Update() is never called - windowless unit tests.
				m_currKeys[key] = m_currKeys[key] || down;
			}
		}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: compile error — `UIKeyboardCapture` is not a member of `aether::ui`.

- [ ] **Step 3: Add the components**

In `src/engine/ui/UiComponents.hpp`, add `#include "ui/UiTextEdit.hpp"` to the includes, then after `UIMask`:

```cpp
	// While the focused element carries this, UiNavigationSystem yields its keys: arrows, Enter
	// and Space reach the element instead of moving or activating focus. Tab still navigates, so
	// there is always a keyboard way out. A marker rather than a flag on UITextBox, so a future
	// dropdown or spinner claims the keyboard the same way without nav learning new types.
	struct UIKeyboardCapture
	{
	};

	// Interactive single-line text field. One entity: the draw builder renders background, text,
	// selection and caret, and UiTextBoxSystem drives editing. Composes a UISelectable for focus
	// (added by the reflection PostSet hook). Editing logic lives in UiTextEdit as pure functions.
	struct UITextBox
	{
		std::string text;
		std::string placeholder;
		std::string fontName = "Roboto";
		std::string allowedChars; // non-empty = whitelist, ANDed with contentType
		TextContentType contentType = TextContentType::Any;
		int maxLength = 0;   // 0 = unlimited
		bool password = false;
		float pixelSize = 20.f;
		float cornerRadius = 4.f;
		float padding = 8.f;
		glm::vec4 bgColor{0.07f, 0.08f, 0.10f, 1.f};
		glm::vec4 bgColorFocused{0.10f, 0.12f, 0.16f, 1.f};
		glm::vec4 textColor{0.90f, 0.94f, 1.f, 1.f};
		glm::vec4 placeholderColor{0.45f, 0.48f, 0.55f, 1.f};
		glm::vec4 caretColor{0.30f, 0.85f, 1.f, 1.f};
		glm::vec4 selectionColor{0.20f, 0.45f, 0.70f, 1.f};

		// runtime, never authored
		bool editing = false;
		bool changed = false;   // text changed this frame
		bool submitted = false; // Enter committed this frame
		bool cancelled = false; // Escape reverted this frame
		bool dragging = false;  // mouse selection drag in progress
		int caret = 0;
		int selectionAnchor = 0;
		float scrollX = 0.f;
		float caretTimer = 0.f;
		float repeatTimer = 0.f;
		int repeatKey = 0;              // GLFW code of the key currently repeating (0 = none)
		std::string committedText;      // snapshot taken on edit entry, restored by Escape
		double lastClickTime = -1.0;    // for double-click word select
	};
```

- [ ] **Step 4: Teach navigation about capture and Tab**

In `src/engine/ui/UiNavigationSystem.cpp`, add to the anonymous namespace:

```cpp
		// Reading order: top-to-bottom, then left-to-right. Rows are compared with a tolerance so
		// controls that are visually on one line do not reorder because of a pixel of drift.
		bool BeforeInReadingOrder(const glm::vec4& a, const glm::vec4& b)
		{
			constexpr float kRowTolerance = 8.f;
			if (std::fabs(a.y - b.y) > kRowTolerance)
			{
				return a.y < b.y;
			}
			return a.x < b.x;
		}
```

Add `#include <algorithm>` if absent.

In `Update`, after `focused` has been resolved and before the keyboard-direction block, insert:

```cpp
		// An element that has claimed the keyboard (a text field being edited) keeps the keys the
		// nav system would otherwise spend: arrows must move its caret, Enter must submit to it.
		// Tab is deliberately exempt - it is the guaranteed keyboard exit from a captured field.
		const bool captured = focused.IsValid() && world.Has<UIKeyboardCapture>(focused);
```

Guard the direction block by changing its opening condition:

```cpp
		if (focused.IsValid() && !captured)
```

Add the Tab handling immediately after that block:

```cpp
		// Tab cycles focus in reading order - what a connect form needs (address, port, connect).
		if (input.IsKeyPressed(Key::Tab) && !cands.empty())
		{
			const bool backwards = input.IsKeyDown(Key::LeftShift) || input.IsKeyDown(Key::RightShift);
			std::vector<Candidate> ordered;
			for (const Candidate& c: cands)
			{
				if (c.interactable)
				{
					ordered.push_back(c);
				}
			}
			std::sort(ordered.begin(), ordered.end(),
			        [](const Candidate& a, const Candidate& b) { return BeforeInReadingOrder(a.rect, b.rect); });
			if (!ordered.empty())
			{
				std::size_t index = 0;
				for (std::size_t i = 0; i < ordered.size(); ++i)
				{
					if (ordered[i].entity == focused)
					{
						index = i;
						break;
					}
				}
				const std::size_t count = ordered.size();
				index = backwards ? (index + count - 1) % count : (index + 1) % count;
				focused = ordered[index].entity;
			}
		}
```

Finally, guard activation:

```cpp
		Entity toActivate{};
		if (!captured && (input.IsKeyPressed(Key::Enter) || input.IsKeyPressed(Key::Space)) && focused.IsValid())
		{
			toActivate = focused;
		}
```

The mouse click branch below it is left unchanged — clicking is how a user leaves a field.

- [ ] **Step 5: Run the tests**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*captured*,*Tab*"
```

Expected: all four PASS.

- [ ] **Step 6: Run the whole suite**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass — existing nav behaviour is unchanged when nothing captures.

- [ ] **Step 7: Commit**

```bash
git add src/engine/ui/UiComponents.hpp src/engine/ui/UiNavigationSystem.cpp src/engine/platform/Input.hpp tests/ui/UiNavigationSystemTests.cpp
git commit -m "Let a focused element capture the keyboard from UI navigation

- Add UIKeyboardCapture and the UITextBox component
- Yield arrows/Enter/Space to a capturing element; add Tab focus cycling"
```

---

### Task 8: UiTextBoxSystem

**Files:**
- Create: `src/engine/ui/UiTextBoxSystem.hpp`, `src/engine/ui/UiTextBoxSystem.cpp`
- Modify: `src/engine/ui/UiRenderer.hpp` (expose the font registry)
- Modify: `src/engine/rendering/RenderingSubsystem.cpp:206` (publish it as a service)
- Modify: `src/app/systems/ScriptComponentSystem.cpp:223-224`

**Interfaces:**
- Consumes: all of `UiTextEdit` (Tasks 5-6); `UITextBox`, `UIKeyboardCapture` (Task 7); `Input::GetTypedChars`, `Input::GetClipboardText`, `Input::SetClipboardText` (Task 4).
- Produces:
  - `FontRegistry& UiRenderer::Fonts()`
  - `void aether::ui::UiTextBoxSystem::Update(World& world, Input& input, FontRegistry* fonts, float time)` — ticked right after `UiWidgetSystem::Update`.

**Font access — read this before writing any code.** The only `FontRegistry` in the process is `UiRenderer::m_fontRegistry`, a *private member* (`src/engine/ui/UiRenderer.hpp:117`). It is not a singleton and is not in the service container, so this system cannot reach it the way it reaches `World` and `Input`. Text metrics are not optional here — click-to-caret and scroll-follows-caret are both measurement. So Step 2 publishes the renderer's registry into the `ServiceContainer` (which is already in scope at the `m_uiRenderer.Init` call site) and the system takes it as a parameter. Do **not** add a `FontRegistry::Instance()` singleton, and do not construct a second registry — it would have no baked atlases.

- [ ] **Step 1: Write the header**

Create `src/engine/ui/UiTextBoxSystem.hpp`:

```cpp
#pragma once

namespace aether
{
	class World;
	class Input;
} // namespace aether

namespace aether::ui
{
	class FontRegistry;

	// Drives UITextBox editing: focus in/out, typed characters, caret keys, clipboard and mouse
	// selection. All of the actual editing rules live in UiTextEdit as pure functions; this is
	// only the marshalling between the ECS + Input and those. Runs after UiNavigationSystem, so
	// it sees this frame's focus and activation.
	//
	// `fonts` may be null (headless, or before the renderer is up): editing still works, but
	// click-to-caret and scroll-follow are measurement and go quiet until a registry arrives.
	class UiTextBoxSystem
	{
	public:
		static void Update(World& world, Input& input, FontRegistry* fonts, float time);
	};
} // namespace aether::ui
```

- [ ] **Step 2: Publish the font registry as a service**

In `src/engine/ui/UiRenderer.hpp`, add a public accessor beside the other public methods:

```cpp
		// The atlases this renderer bakes are the only ones in the process. UI systems that
		// measure text (text-box hit-testing, scrolling) need them, so hand out a reference
		// rather than letting anyone build a second, empty registry.
		[[nodiscard]] FontRegistry& Fonts()
		{
			return m_fontRegistry;
		}
```

In `src/engine/rendering/RenderingSubsystem.cpp`, immediately after the `m_uiRenderer.Init(...)` call on line 206:

```cpp
		services.Register<ui::FontRegistry>(m_uiRenderer.Fonts());
```

`ServiceContainer::Register` takes a reference (`src/engine/utils/ServiceContainer.hpp:20`) and stores a non-owning pointer; `m_uiRenderer` outlives the container's use here.

- [ ] **Step 3: Write the implementation**

Create `src/engine/ui/UiTextBoxSystem.cpp`:

```cpp
#include "ui/UiTextBoxSystem.hpp"

#include <algorithm>
#include <cmath>

#include <entt/entt.hpp>

#include "platform/Input.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiTextEdit.hpp"

namespace aether::ui
{
	namespace
	{
		// Held keys repeat like an OS key-repeat: one immediate action, a pause, then a stream.
		constexpr float kRepeatDelay = 0.4f;
		constexpr float kRepeatRate = 0.03f;
		constexpr double kDoubleClickSeconds = 0.35;

		TextEditState ToEditState(const UITextBox& box)
		{
			TextEditState s;
			s.text = box.text;
			s.caret = box.caret;
			s.selectionAnchor = box.selectionAnchor;
			s.scrollX = box.scrollX;
			return s;
		}

		void FromEditState(const TextEditState& s, UITextBox& box)
		{
			box.text = s.text;
			box.caret = s.caret;
			box.selectionAnchor = s.selectionAnchor;
			box.scrollX = s.scrollX;
		}

		TextEditLimits LimitsOf(const UITextBox& box)
		{
			TextEditLimits limits;
			limits.contentType = box.contentType;
			limits.allowedChars = box.allowedChars;
			limits.maxLength = box.maxLength;
			return limits;
		}

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

		void SetCapture(World& world, Entity e, bool capture)
		{
			const bool has = world.Has<UIKeyboardCapture>(e);
			if (capture && !has)
			{
				world.Emplace<UIKeyboardCapture>(e);
			}
			else if (!capture && has)
			{
				world.Remove<UIKeyboardCapture>(e);
			}
		}

		// True on the down-edge, and again on the repeat schedule while held. One key repeats at a
		// time - the last one pressed - which is what a keyboard does.
		bool KeyAction(Input& input, UITextBox& box, Key key, float dt)
		{
			const auto code = static_cast<int>(key);
			if (input.IsKeyPressed(key))
			{
				box.repeatKey = code;
				box.repeatTimer = kRepeatDelay;
				return true;
			}
			if (box.repeatKey != code || !input.IsKeyDown(key))
			{
				return false;
			}
			box.repeatTimer -= dt;
			if (box.repeatTimer <= 0.f)
			{
				box.repeatTimer = kRepeatRate;
				return true;
			}
			return false;
		}
	} // namespace

	void UiTextBoxSystem::Update(World& world, Input& input, FontRegistry* fonts, float time)
	{
		// Frame delta from the absolute time this system is handed, matching UiWidgetSystem.
		// Clamped so a pause or scene switch cannot jump the caret blink or key repeat.
		static float s_prevTime = time;
		const float dt = std::clamp(time - s_prevTime, 0.f, 0.1f);
		s_prevTime = time;

		const glm::vec2 mouse = input.GetMousePos();
		const bool mouseDown = input.IsMouseButtonDown(MouseButton::Left);
		const bool mousePressed = input.IsMouseButtonPressed(MouseButton::Left);
		const bool shift = input.IsKeyDown(Key::LeftShift) || input.IsKeyDown(Key::RightShift);
		const bool ctrl = input.IsKeyDown(Key::LeftCtrl) || input.IsKeyDown(Key::RightCtrl);

		world.View<UITextBox, UIRect>().each(
		        [&](entt::entity ent, UITextBox& box, UIRect& rect)
		        {
			        const Entity e = World::FromEntt(ent);
			        box.changed = false;
			        box.submitted = false;
			        box.cancelled = false;

			        if (ecs::HasDisabledAncestor(world, e))
			        {
				        box.editing = false;
				        box.dragging = false;
				        SetCapture(world, e, false);
				        return;
			        }

			        const glm::vec4 r = rect.resolvedRect;
			        const float innerX = r.x + box.padding;
			        const float innerW = std::max(r.z - 2.f * box.padding, 1.f);
			        const bool hovered = mouse.x >= r.x && mouse.x <= r.x + r.z && mouse.y >= r.y && mouse.y <= r.y + r.w;

			        // Losing focus commits and releases the keyboard.
			        if (box.editing && !Focused(world, e))
			        {
				        box.editing = false;
				        box.dragging = false;
				        SetCapture(world, e, false);
			        }

			        // Activation (click, or Enter/Space while focused) starts editing.
			        if (!box.editing && Activated(world, e))
			        {
				        box.editing = true;
				        box.committedText = box.text;
				        box.caretTimer = 0.f;
				        box.repeatKey = 0;
				        SetCapture(world, e, true);
				        TextEditState s = ToEditState(box);
				        SelectAll(s); // entering a field selects it, so typing replaces
				        FromEditState(s, box);
			        }

			        if (!box.editing)
			        {
				        return;
			        }

			        const FontAsset* font = fonts != nullptr ? fonts->Load(box.fontName) : nullptr;
			        TextEditState s = ToEditState(box);
			        bool textChanged = false;

			        // ── Mouse: click places the caret, drag extends, double-click selects a word ──
			        if (font != nullptr && mousePressed && hovered)
			        {
				        const std::string display = DisplayText(s.text, box.password);
				        const float localX = mouse.x - innerX + s.scrollX;
				        const int index = CaretFromPixelX(*font, display, box.pixelSize, localX);
				        const double now = static_cast<double>(time);
				        if (box.lastClickTime >= 0.0 && now - box.lastClickTime < kDoubleClickSeconds)
				        {
					        SelectWordAt(s, index);
				        }
				        else
				        {
					        s.caret = index;
					        if (!shift)
					        {
						        ClearSelection(s);
					        }
					        box.dragging = true;
				        }
				        box.lastClickTime = now;
				        box.caretTimer = 0.f;
			        }
			        else if (font != nullptr && box.dragging && mouseDown)
			        {
				        const std::string display = DisplayText(s.text, box.password);
				        s.caret = CaretFromPixelX(*font, display, box.pixelSize, mouse.x - innerX + s.scrollX);
			        }
			        if (!mouseDown)
			        {
				        box.dragging = false;
			        }

			        // ── Clipboard ───────────────────────────────────────────────────────────────
			        if (ctrl && input.IsKeyPressed(Key::A))
			        {
				        SelectAll(s);
			        }
			        if (ctrl && input.IsKeyPressed(Key::C) && HasSelection(s))
			        {
				        input.SetClipboardText(SelectedText(s));
			        }
			        if (ctrl && input.IsKeyPressed(Key::X) && HasSelection(s))
			        {
				        input.SetClipboardText(SelectedText(s));
				        textChanged = DeleteSelection(s) || textChanged;
			        }
			        if (ctrl && input.IsKeyPressed(Key::V))
			        {
				        textChanged = InsertText(s, LimitsOf(box), input.GetClipboardText()) || textChanged;
			        }

			        // ── Typed characters ────────────────────────────────────────────────────────
			        // Ctrl chords are shortcuts, not text; GLFW does not emit chars for them anyway,
			        // but skipping keeps a stray char from a chord out of the field.
			        if (!ctrl && !input.GetTypedChars().empty())
			        {
				        textChanged = InsertText(s, LimitsOf(box), input.GetTypedChars()) || textChanged;
			        }

			        // ── Caret and deletion ──────────────────────────────────────────────────────
			        if (KeyAction(input, box, Key::Left, dt))
			        {
				        MoveCaret(s, ctrl ? CaretMove::WordLeft : CaretMove::Left, shift);
			        }
			        if (KeyAction(input, box, Key::Right, dt))
			        {
				        MoveCaret(s, ctrl ? CaretMove::WordRight : CaretMove::Right, shift);
			        }
			        if (KeyAction(input, box, Key::Home, dt))
			        {
				        MoveCaret(s, CaretMove::Home, shift);
			        }
			        if (KeyAction(input, box, Key::End, dt))
			        {
				        MoveCaret(s, CaretMove::End, shift);
			        }
			        if (KeyAction(input, box, Key::Backspace, dt))
			        {
				        textChanged = DeleteBackward(s, ctrl) || textChanged;
			        }
			        if (KeyAction(input, box, Key::Delete, dt))
			        {
				        textChanged = DeleteForward(s, ctrl) || textChanged;
			        }

			        // ── Commit / cancel ─────────────────────────────────────────────────────────
			        bool leaveEditing = false;
			        if (input.IsKeyPressed(Key::Enter) || input.IsKeyPressed(Key::KpEnter))
			        {
				        box.submitted = true;
				        leaveEditing = true;
			        }
			        else if (input.IsKeyPressed(Key::Escape))
			        {
				        if (s.text != box.committedText)
				        {
					        s.text = box.committedText;
					        s.caret = static_cast<int>(s.text.size());
					        ClearSelection(s);
					        textChanged = true;
				        }
				        box.cancelled = true;
				        leaveEditing = true;
			        }
			        else if (input.IsKeyPressed(Key::Tab))
			        {
				        leaveEditing = true; // navigation already moved focus on this key
			        }

			        if (font != nullptr)
			        {
				        ScrollToCaret(s, *font, box.pixelSize, innerW, box.password);
			        }
			        FromEditState(s, box);

			        box.changed = textChanged;
			        box.caretTimer += dt;
			        if (leaveEditing)
			        {
				        box.editing = false;
				        box.dragging = false;
				        box.repeatKey = 0;
				        box.committedText = box.text;
				        SetCapture(world, e, false);
			        }
		        });
	}
} // namespace aether::ui
```

- [ ] **Step 4: Tick it**

In `src/app/systems/ScriptComponentSystem.cpp`, add `#include "ui/FontRegistry.hpp"` and `#include "ui/UiTextBoxSystem.hpp"`, then immediately after the `UiWidgetSystem::Update` line:

```cpp
			aether::ui::UiTextBoxSystem::Update(world, *sceneCtx->input, m_services.TryGet<aether::ui::FontRegistry>(), static_cast<float>(sceneCtx->elapsedTime));
```

`m_services` is already the member `ScriptComponentSystem::Update` uses to reach `SceneContext`, so nothing new is threaded through.

- [ ] **Step 5: Build**

```bash
cmake --build build/ninja-clang --target Editor EngineTests
```

Expected: success.

- [ ] **Step 6: Run the suite**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine/ui/UiTextBoxSystem.hpp src/engine/ui/UiTextBoxSystem.cpp src/engine/ui/UiRenderer.hpp src/engine/rendering/RenderingSubsystem.cpp src/app/systems/ScriptComponentSystem.cpp
git commit -m "Drive text box editing from input

- Add UiTextBoxSystem, marshalling ECS and Input into the text editing core
- Publish the renderer's font registry as a service so UI systems can measure text"
```

---

### Task 9: Render the text box

**Files:**
- Modify: `src/engine/ui/UiDrawBuilder.cpp` (add `EmitTextBox`, call it from `Walk`)
- Test: `tests/ui/UiDrawBuilderTests.cpp`

**Interfaces:**
- Consumes: `UITextBox` (Task 7), `TextWidth`/`CaretToPixelX`/`DisplayText` (Tasks 5-6), `kFlagClip` and the intersect-not-overwrite stamping rule (Tasks 1, 3).
- Produces: draw commands, in order — background rect, selection rect (when a selection exists), glyphs, caret rect (only while editing and on the blink's visible half).

- [ ] **Step 1: Write the failing tests**

Append to `tests/ui/UiDrawBuilderTests.cpp`:

```cpp
TEST_CASE("Text box emits a background and clips its glyphs to the padded inner rect")
{
	World w;
	ui::FontRegistry fonts;
	fonts.InjectForTest("Roboto", MakeMonoFont());

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity field = w.Create();
	auto& fr = w.Emplace<ui::UIRect>(field);
	fr.resolvedRect = {100, 100, 200, 40};
	auto& box = w.Emplace<ui::UITextBox>(field);
	box.text = "AB";
	box.pixelSize = 48.f;
	box.padding = 8.f;
	w.Emplace<HierarchyComponent>(field);
	ecs::SetParent(w, field, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	std::vector<ui::UiMaterialDraw> materials;
	ui::BuildDrawCommands(w, cmds, materials, &fonts, nullptr);

	REQUIRE(cmds.size() == 3); // background + two glyphs

	CHECK(cmds[0].type == ui::kShapeRect);
	CHECK((cmds[0].flags & ui::kFlagClip) == 0u); // the box IS the boundary; it is not clipped
	CHECK(cmds[0].data0.z == doctest::Approx(200));

	for (std::size_t i = 1; i < cmds.size(); ++i)
	{
		CHECK(cmds[i].type == ui::kShapeSdfGlyph);
		CHECK((cmds[i].flags & ui::kFlagClip) != 0u);
		CHECK(cmds[i].clipRect.x == doctest::Approx(108)); // 100 + 8 padding
		CHECK(cmds[i].clipRect.z == doctest::Approx(184)); // 200 - 2 * 8
	}
}

TEST_CASE("Text box emits a caret only while editing")
{
	World w;
	ui::FontRegistry fonts;
	fonts.InjectForTest("Roboto", MakeMonoFont());

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity field = w.Create();
	auto& fr = w.Emplace<ui::UIRect>(field);
	fr.resolvedRect = {100, 100, 200, 40};
	auto& box = w.Emplace<ui::UITextBox>(field);
	box.text = "AB";
	box.pixelSize = 48.f;
	box.caret = 2;
	w.Emplace<HierarchyComponent>(field);
	ecs::SetParent(w, field, canvas);

	std::vector<ui::UiDrawCommand> notEditing;
	std::vector<ui::UiMaterialDraw> materials;
	ui::BuildDrawCommands(w, notEditing, materials, &fonts, nullptr);
	const std::size_t idleCount = notEditing.size();

	box.editing = true;
	box.caretTimer = 0.f; // blink's visible half
	std::vector<ui::UiDrawCommand> editing;
	ui::BuildDrawCommands(w, editing, materials, &fonts, nullptr);

	CHECK(editing.size() == idleCount + 1);
	CHECK(editing.back().type == ui::kShapeRect);
	CHECK(editing.back().data0.x == doctest::Approx(156)); // 108 inner + 2 glyphs * 24 px
}

TEST_CASE("Text box shows the placeholder when empty")
{
	World w;
	ui::FontRegistry fonts;
	fonts.InjectForTest("Roboto", MakeMonoFont());

	Entity canvas = w.Create();
	w.Emplace<ui::UICanvas>(canvas);
	auto& cr = w.Emplace<ui::UIRect>(canvas);
	cr.resolvedRect = {0, 0, 1000, 800};
	w.Emplace<HierarchyComponent>(canvas);

	Entity field = w.Create();
	auto& fr = w.Emplace<ui::UIRect>(field);
	fr.resolvedRect = {100, 100, 200, 40};
	auto& box = w.Emplace<ui::UITextBox>(field);
	box.text = "";
	box.placeholder = "AB";
	box.pixelSize = 48.f;
	box.placeholderColor = {0.5f, 0.5f, 0.5f, 1.f};
	w.Emplace<HierarchyComponent>(field);
	ecs::SetParent(w, field, canvas);

	std::vector<ui::UiDrawCommand> cmds;
	std::vector<ui::UiMaterialDraw> materials;
	ui::BuildDrawCommands(w, cmds, materials, &fonts, nullptr);

	REQUIRE(cmds.size() == 3);
	CHECK(cmds[1].color.r == doctest::Approx(0.5f)); // drawn in the placeholder colour
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build/ninja-clang --target EngineTests
```

Expected: the tests compile but fail — a `UITextBox` emits nothing, so `cmds.size()` is 0.

- [ ] **Step 3: Implement EmitTextBox**

In `src/engine/ui/UiDrawBuilder.cpp`, add `#include "ui/UiTextEdit.hpp"` and, after `EmitButton`:

```cpp
	static void EmitTextBox(World& world, Entity e, const UIRect& rect, const UITextBox& box, FontRegistry& fonts, int& layer, std::vector<UiDrawCommand>& out)
	{
		const glm::vec4 r = rect.resolvedRect;
		const bool focused = WidgetFocused(world, e);

		UiDrawCommand bg;
		bg.type = kShapeRect;
		bg.data0 = r;
		bg.data1.x = box.cornerRadius;
		bg.color = (focused || box.editing) ? box.bgColorFocused : box.bgColor;
		bg.layer = layer++;
		out.push_back(bg);

		const glm::vec4 inner{r.x + box.padding, r.y + box.padding, std::max(r.z - 2.f * box.padding, 0.f), std::max(r.w - 2.f * box.padding, 0.f)};

		const FontAsset* font = fonts.Load(box.fontName);
		if (font == nullptr)
		{
			return;
		}

		const bool showPlaceholder = box.text.empty();
		const std::string display = showPlaceholder ? box.placeholder : DisplayText(box.text, box.password);
		const float originX = inner.x - box.scrollX;

		// Everything past the background is clipped to the padded inner rect, so long text
		// scrolls under the edges instead of spilling out of the field. The builder's clip
		// stack intersects rather than overwrites, so an ancestor UIMask still applies.
		const std::size_t clipBegin = out.size();

		if (!showPlaceholder && box.selectionAnchor != box.caret)
		{
			const int begin = std::min(box.caret, box.selectionAnchor);
			const int end = std::max(box.caret, box.selectionAnchor);
			const float x0 = originX + CaretToPixelX(*font, display, box.pixelSize, begin);
			const float x1 = originX + CaretToPixelX(*font, display, box.pixelSize, end);

			UiDrawCommand sel;
			sel.type = kShapeRect;
			sel.data0 = {x0, inner.y, std::max(x1 - x0, 1.f), inner.w};
			sel.color = box.selectionColor;
			sel.layer = layer++;
			out.push_back(sel);
		}

		// Left-aligned, vertically centred, never wrapped: the run box starts at the scrolled
		// origin and is exactly as wide as the text, so ShapeText lays it out in one line.
		const glm::vec4 runRect{originX, inner.y, TextWidth(*font, display, box.pixelSize), inner.w};
		EmitTextRun(runRect, display, box.fontName, box.pixelSize, showPlaceholder ? box.placeholderColor : box.textColor, UIText::HAlign::Left, UIText::VAlign::Middle, false, fonts, layer, out);

		// Caret: on for the first half of each second, so it blinks without a timer service.
		if (box.editing && std::fmod(box.caretTimer, 1.f) < 0.5f)
		{
			UiDrawCommand caret;
			caret.type = kShapeRect;
			caret.data0 = {originX + CaretToPixelX(*font, display, box.pixelSize, box.caret), inner.y, 2.f, inner.w};
			caret.color = box.caretColor;
			caret.layer = layer++;
			out.push_back(caret);
		}

		for (std::size_t i = clipBegin; i < out.size(); ++i)
		{
			out[i].clipRect = inner;
			out[i].flags |= kFlagClip;
		}
	}
```

In `Walk`, inside the `fonts != nullptr` block that handles `UIButton`, add:

```cpp
				if (const auto* textBox = world.TryGet<UITextBox>(entity))
				{
					EmitTextBox(world, entity, *rect, *textBox, *fonts, layer, out);
				}
```

- [ ] **Step 4: Run the tests**

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe -tc="*Text box*"
```

Expected: all three PASS.

- [ ] **Step 5: Run the whole suite**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass.

- [ ] **Step 6: Commit**

```bash
git add src/engine/ui/UiDrawBuilder.cpp tests/ui/UiDrawBuilderTests.cpp
git commit -m "Render the text box: background, selection, text and caret"
```

---

### Task 10: Authoring — reflection, entity factory, editor summaries

**Files:**
- Modify: `src/app/scene/reflection/MoreComponents.reflect.cpp`
- Modify: `src/engine/ui/UiEntities.hpp`, `src/engine/ui/UiEntities.cpp`
- Modify: `src/app/debug/ComponentDrawers.cpp:110-125`
- Modify: `src/app/debug/UiCanvasPanel.cpp:808-830`

**Interfaces:**
- Consumes: `UITextBox` (Task 7).
- Produces: `Entity aether::ui::CreateTextBoxEntity(World& world, Entity canvas)`; a "UI Text Box" palette/inspector entry; MCP `add_component` support for `UiTextBoxComponent` with fields `text`, `placeholder`, `font`, `content_type`, `allowed_chars`, `max_length`, `password`, `pixel_size`, `corner_radius`, `padding`, and the six colours.

- [ ] **Step 1: Add the content-type enum table and component block**

In `src/app/scene/reflection/MoreComponents.reflect.cpp`, add to the alias block:

```cpp
using UiTextBoxComponent = aether::ui::UITextBox;
```

In the anonymous namespace, beside the other enum tables:

```cpp
	const reflect::EnumTable& UiContentTypeEnum()
	{
		static const reflect::EnumTable table{{
		        {"any", static_cast<int>(aether::ui::TextContentType::Any)},
		        {"integer", static_cast<int>(aether::ui::TextContentType::Integer)},
		        {"decimal", static_cast<int>(aether::ui::TextContentType::Decimal)},
		        {"alphanumeric", static_cast<int>(aether::ui::TextContentType::Alphanumeric)},
		        {"ip_address", static_cast<int>(aether::ui::TextContentType::IpAddress)},
		}};
		return table;
	}
```

After the `UiButtonComponent` block:

```cpp
AE_COMPONENT(UiTextBoxComponent, "UI Text Box", "UI", ICON_FA_KEYBOARD)
AE_FIELD_N("text", text, String)
AE_FIELD_N("placeholder", placeholder, String)
AE_FIELD_N("font", fontName, String)
AE_FIELD_ENUM("content_type", contentType, UiContentTypeEnum())
AE_FIELD_N("allowed_chars", allowedChars, String)
AE_FIELD_N("max_length", maxLength, Int)
AE_FIELD_N("password", password, Bool)
AE_FIELD_N("pixel_size", pixelSize, Float)
AE_FIELD_N("corner_radius", cornerRadius, Float)
AE_FIELD_N("padding", padding, Float)
AE_FIELD_N("bg_color", bgColor, Color4)
AE_FIELD_N("bg_color_focused", bgColorFocused, Color4)
AE_FIELD_N("text_color", textColor, Color4)
AE_FIELD_N("placeholder_color", placeholderColor, Color4)
AE_FIELD_N("caret_color", caretColor, Color4)
AE_FIELD_N("selection_color", selectionColor, Color4)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
```

`ICON_FA_KEYBOARD` is defined at `src/app/debug/Icons.hpp:758`.

- [ ] **Step 2: Add the entity factory**

In `src/engine/ui/UiEntities.hpp`, after `CreateButtonEntity`:

```cpp
	Entity CreateTextBoxEntity(World& world, Entity canvas);
```

In `src/engine/ui/UiEntities.cpp`, after `CreateButtonEntity`:

```cpp
	Entity CreateTextBoxEntity(World& world, Entity canvas)
	{
		canvas = EnsureCanvas(world, canvas);

		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = "TextBox"});
		world.Emplace<HierarchyComponent>(e);

		auto& rect = world.Emplace<UIRect>(e);
		rect.anchorMin = {0.5f, 0.5f};
		rect.anchorMax = {0.5f, 0.5f};
		rect.offsetMin = {-140.f, -18.f};
		rect.offsetMax = {140.f, 18.f};

		auto& box = world.Emplace<UITextBox>(e);
		box.placeholder = "Enter text";
		world.Emplace<UISelectable>(e);

		ecs::SetParent(world, e, canvas);
		return e;
	}
```

- [ ] **Step 3: Add the editor summaries**

In `src/app/debug/ComponentDrawers.cpp`, in `EntityKindBadge`, alongside the other widget checks (before the `UIImage`/`UIRect` catch-all):

```cpp
		if (world.Has<ui::UITextBox>(entity))
		{
			return {ICON_FA_KEYBOARD, ToImVec4(colors::Info)};
		}
```

In `src/app/debug/UiCanvasPanel.cpp`, after the `UIProgressBar` preview block:

```cpp
			if (const auto* textBox = world.TryGet<ui::UITextBox>(element.entity))
			{
				drawList->AddRectFilled(element.min, element.max, ToU32(textBox->bgColor), textBox->cornerRadius * zoom);
				// The canvas preview does not shape text; a caret tick is enough to read the
				// element as a field rather than a plain panel.
				const float pad = textBox->padding * zoom;
				drawList->AddLine(ImVec2(element.min.x + pad, element.min.y + pad), ImVec2(element.min.x + pad, element.max.y - pad), ToU32(textBox->caretColor), 2.f);
			}
```

Use the same icon token you settled on in Step 1.

- [ ] **Step 4: Build and verify serialization round-trips**

```bash
cmake --build build/ninja-clang --target Editor EngineTests && ./build/ninja-clang/tests/EngineTests.exe
```

Expected: all pass. `SceneSerializerTests` walks the registry, so a malformed `AE_COMPONENT` block shows up here.

- [ ] **Step 5: Commit**

```bash
git add src/app/scene/reflection/MoreComponents.reflect.cpp src/engine/ui/UiEntities.hpp src/engine/ui/UiEntities.cpp src/app/debug/ComponentDrawers.cpp src/app/debug/UiCanvasPanel.cpp
git commit -m "Make the text box authorable from the editor and MCP"
```

---

### Task 11: Scripting surface

**Files:**
- Modify: `src/app/scripting/interop/UiExports.cpp`
- Modify: `src/app/scripting/interop/InputExports.cpp`
- Modify: `managed/AetherCore/Internal/Native.cs`
- Modify: `managed/AetherCore/Ui.cs`
- Modify: `managed/AetherCore/Input.cs`

**Interfaces:**
- Consumes: `UITextBox` (Task 7), `CreateTextBoxEntity` (Task 10), `Input` clipboard (Task 4).
- Produces, in C#:
  - `Ui.CreateTextBox(Entity canvas = default)`, `Ui.GetTextBoxText(Entity)`, `Ui.SetTextBoxText(Entity, string)`, `Ui.SetPlaceholder(Entity, string)`, `Ui.SetContentType(Entity, UiContentType)`, `Ui.WasSubmitted(Entity)`, `Ui.WasCancelled(Entity)`, `Ui.IsEditing(Entity)`, `Ui.BeginEdit(Entity)`
  - `enum UiContentType { Any, Integer, Decimal, Alphanumeric, IpAddress }`
  - `Input.Clipboard { get; set; }`
  - `Ui.WasChanged` extended to report text-box changes.

Keep `UiExports.cpp` runtime-safe: no `ComponentCatalog` or editor headers — it is compiled into `GameRuntime`.

- [ ] **Step 1: Add the native exports**

In `src/app/scripting/interop/UiExports.cpp`, after the button-label exports:

```cpp
AE_SCRIPT_API std::uint32_t aether_ui_create_text_box(std::uint32_t canvasId)
{
	auto& world = ActiveWorld();
	return aether::ui::CreateTextBoxEntity(world, ResolveCanvas(world, canvasId)).id;
}

AE_SCRIPT_API std::int32_t aether_ui_get_text_box_text(std::uint32_t id, char* buf, std::int32_t bufLen)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	if (b == nullptr || buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(b->text.size()));
	std::memcpy(buf, b->text.data(), static_cast<std::size_t>(n));
	return n;
}

AE_SCRIPT_API void aether_ui_set_text_box_text(std::uint32_t id, const char* text)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id}))
	{
		b->text = text != nullptr ? text : "";
		// A programmatic set puts the caret at the end and drops any selection, so the next
		// keystroke appends instead of replacing text the player never chose.
		b->caret = static_cast<int>(b->text.size());
		b->selectionAnchor = b->caret;
		b->scrollX = 0.f;
	}
}

AE_SCRIPT_API void aether_ui_set_text_box_placeholder(std::uint32_t id, const char* text)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id}))
	{
		b->placeholder = text != nullptr ? text : "";
	}
}

AE_SCRIPT_API void aether_ui_set_text_box_content_type(std::uint32_t id, std::int32_t contentType)
{
	if (auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id}))
	{
		b->contentType = static_cast<aether::ui::TextContentType>(contentType);
	}
}

AE_SCRIPT_API std::int32_t aether_ui_was_submitted(std::uint32_t id)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	return (b != nullptr && b->submitted) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_ui_was_cancelled(std::uint32_t id)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	return (b != nullptr && b->cancelled) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_ui_is_editing(std::uint32_t id)
{
	const auto* b = ActiveWorld().TryGet<aether::ui::UITextBox>(aether::Entity{id});
	return (b != nullptr && b->editing) ? 1 : 0;
}

AE_SCRIPT_API void aether_ui_begin_edit(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (auto* sel = world.TryGet<aether::ui::UISelectable>(e))
	{
		// Route through activation so the system runs its normal entry path (snapshot, select
		// all, capture) rather than duplicating it here.
		sel->focused = true;
		sel->activated = true;
	}
}
```

Add `#include "ui/UiTextEdit.hpp"` to the file's includes.

In the existing `aether_ui_was_changed`, add before the final `return 0;`:

```cpp
	if (const auto* b = world.TryGet<aether::ui::UITextBox>(e); b != nullptr && b->changed)
	{
		return 1;
	}
```

The clipboard exports go in `src/app/scripting/interop/InputExports.cpp`, not here — that file already owns every `aether_input_*` symbol and reaches input via `ActiveContext().input`. Add there:

```cpp
AE_SCRIPT_API std::int32_t aether_input_get_clipboard(char* buf, std::int32_t bufLen)
{
	if (buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const std::string text = ActiveContext().input->GetClipboardText();
	const std::int32_t n = std::min<std::int32_t>(bufLen, static_cast<std::int32_t>(text.size()));
	std::memcpy(buf, text.data(), static_cast<std::size_t>(n));
	return n;
}

AE_SCRIPT_API void aether_input_set_clipboard(const char* text)
{
	ActiveContext().input->SetClipboardText(text != nullptr ? text : "");
}
```

Add `#include <algorithm>`, `#include <cstring>` and `#include <string>` to `InputExports.cpp` if they are not already there. Both files already exist, so no CMake reconfigure is needed (a *brand-new* `src/app/*.cpp` builds into `GameRuntime` but not `Editor` until you reconfigure — that trap does not apply here).

- [ ] **Step 2: Declare them in Native.cs**

In `managed/AetherCore/Internal/Native.cs`, beside the other UI declarations:

```csharp
    [LibraryImport(Lib)]
    internal static partial uint aether_ui_create_text_box(uint canvasId);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_get_text_box_text(uint id, byte* buf, int bufLen);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_text_box_text(uint id, string text);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_ui_set_text_box_placeholder(uint id, string text);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_set_text_box_content_type(uint id, int contentType);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_was_submitted(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_was_cancelled(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_ui_is_editing(uint id);

    [LibraryImport(Lib)]
    internal static partial void aether_ui_begin_edit(uint id);

    [LibraryImport(Lib)]
    internal static partial int aether_input_get_clipboard(byte* buf, int bufLen);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial void aether_input_set_clipboard(string text);
```

- [ ] **Step 3: Add the C# surface**

In `managed/AetherCore/Ui.cs`, add the enum beside `UiHAlign`:

```csharp
/// <summary>Characters a text box accepts. ANDed with its allowed-characters string.</summary>
public enum UiContentType { Any = 0, Integer = 1, Decimal = 2, Alphanumeric = 3, IpAddress = 4 }
```

And a section after the widget accessors:

```csharp
    // ── Text box ──────────────────────────────────────────────────────────────────
    // A single-line editable field. Editing is modal: the player clicks it (or presses
    // Enter while it is focused) to start, and Enter/Escape/Tab/clicking away ends it.
    // Poll WasSubmitted to act on a committed value.

    /// <summary>Create an editable text box under <paramref name="canvas"/>.</summary>
    public static Entity CreateTextBox(Entity canvas = default) => new(Native.aether_ui_create_text_box(canvas.Id));

    /// <summary>The text box's current string (empty if the entity has none).</summary>
    public static unsafe string GetTextBoxText(Entity e)
    {
        Span<byte> buffer = stackalloc byte[512];
        fixed (byte* ptr = buffer)
        {
            int written = Native.aether_ui_get_text_box_text(e.Id, ptr, buffer.Length);
            return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
        }
    }

    /// <summary>Replace the text box's string; the caret moves to the end (ASCII only).</summary>
    public static void SetTextBoxText(Entity e, string text) => Native.aether_ui_set_text_box_text(e.Id, text);

    /// <summary>The greyed-out hint shown while the box is empty.</summary>
    public static void SetPlaceholder(Entity e, string text) => Native.aether_ui_set_text_box_placeholder(e.Id, text);

    /// <summary>Restrict which characters the box accepts.</summary>
    public static void SetContentType(Entity e, UiContentType type) => Native.aether_ui_set_text_box_content_type(e.Id, (int)type);

    /// <summary>True on the frame the player pressed Enter to commit the field.</summary>
    public static bool WasSubmitted(Entity e) => Native.aether_ui_was_submitted(e.Id) != 0;

    /// <summary>True on the frame the player pressed Escape, reverting to the value on entry.</summary>
    public static bool WasCancelled(Entity e) => Native.aether_ui_was_cancelled(e.Id) != 0;

    /// <summary>True while the box owns the keyboard.</summary>
    public static bool IsEditing(Entity e) => Native.aether_ui_is_editing(e.Id) != 0;

    /// <summary>Focus the box and start editing, as a click would.</summary>
    public static void BeginEdit(Entity e) => Native.aether_ui_begin_edit(e.Id);
```

In `managed/AetherCore/Input.cs`, add (and `using System.Text;` if absent):

```csharp
    /// <summary>The OS clipboard's text contents.</summary>
    public static unsafe string Clipboard
    {
        get
        {
            Span<byte> buffer = stackalloc byte[1024];
            fixed (byte* ptr = buffer)
            {
                int written = Native.aether_input_get_clipboard(ptr, buffer.Length);
                return written > 0 ? Encoding.UTF8.GetString(ptr, written) : string.Empty;
            }
        }
        set => Native.aether_input_set_clipboard(value ?? string.Empty);
    }
```

- [ ] **Step 4: Build both sides**

```bash
cmake --build build/ninja-clang --target Editor GameRuntime
```

Expected: success, including the managed assemblies (CMake builds the `.csproj` directly). An `EntryPointNotFoundException` at runtime for `AetherHost` means CMake needs a reconfigure — no new `.cpp` was added here, so this should not occur.

- [ ] **Step 5: Commit**

```bash
git add src/app/scripting/interop/UiExports.cpp src/app/scripting/interop/InputExports.cpp managed/AetherCore/Internal/Native.cs managed/AetherCore/Ui.cs managed/AetherCore/Input.cs
git commit -m "Expose the text box and clipboard to scripts"
```

---

### Task 12: End-to-end verification

Everything so far is unit-tested or compiled. This task proves it works on screen and under automation — nothing is claimed complete until these produce output.

**Files:**
- No production changes expected. Fix anything this surfaces in the task that owns it, then re-run.

- [ ] **Step 1: Full build from clean shader state**

```bash
cmake --build build/ninja-clang --target CompileShaders EngineAssetsPak Editor GameRuntime EngineTests
```

Expected: all targets succeed.

- [ ] **Step 2: Full test suite**

```bash
./build/ninja-clang/tests/EngineTests.exe
```

Expected: 0 failures. Record the assertion/test-case counts in the commit or handoff — that is the evidence the work is green.

- [ ] **Step 3: Author a field in the editor**

Launch the editor from the build-tree root (`shaders://` is CWD-relative):

```bash
cd build/ninja-clang && ./Editor.exe
```

Then, via MCP against the running editor:
1. `new_scene` with `kind=2d`.
2. `create_entity` named `Canvas` with a `UiCanvasComponent`.
3. `create_entity` named `Address` parented to it, with a `UiTextBoxComponent` (`placeholder="127.0.0.1:7777"`, `content_type="ip_address"`, `max_length=21`).
4. `create_entity` named `Connect` with a `UiButtonComponent` below it.
5. `play`, then `screenshot`.

Verify in the screenshot: the placeholder renders greyed; clicking the field highlights it; typing digits and dots enters them and letters are rejected; typing past the right edge scrolls the text under the border rather than spilling out; Tab moves focus to the button; Escape restores the value the field had on entry.

- [ ] **Step 4: Drive it headless**

With the scene playing, use MCP `send_input`:

```
send_input {"text": "192.168.0.50:7777"}
send_input {"down": ["enter"]}
send_input {"up": ["enter"]}
```

Then `screenshot` and confirm the committed value is shown. This exercises the synthetic-char path added in Task 4 and is the regression test a future `.seq` playtest will use.

- [ ] **Step 5: Confirm the shader migration did not regress INKBOUND**

Open the INKBOUND project, load a scene that uses `ui_glitch_text` or `ui_dialogue_text`, play it and screenshot. Expected: the material effects render exactly as before — the shared-header migration changed only where `VSOutput` is declared.

- [ ] **Step 6: Commit any fixes and merge**

```bash
git status
```

If clean, fast-forward merge to master and push, per the project's standing practice. If not, fix in the owning task and re-run Steps 1-5 before merging.

---

## Verification Summary

Every task ends with a runnable command and an expected result. The suite command is:

```bash
cmake --build build/ninja-clang --target EngineTests && ./build/ninja-clang/tests/EngineTests.exe
```

Do not report any task complete without pasting the actual output of its verification step.
