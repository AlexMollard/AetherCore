# In-Engine UI Backend + UI Editor - Design

Date: 2026-07-07
Status: Approved (design). Ready for implementation planning.

## Goal

Add a **retained-mode UI** to AetherCore that authors like the rest of the engine:
UI elements are ECS entities, edited in the inspector/hierarchy, serialized into the
scene TOML, and rendered by an in-engine GPU backend. Scope is deliberately minimal -
**text, quads/images, and anchoring** - plus a dedicated **UI Canvas editor panel** and a
live **viewport overlay** preview.

Ship alongside it two scene-workflow conveniences the editor is missing today: **quick-save
(Ctrl+S)** to the currently-loaded scene, and **New Scene** producing a default physics
scene (a dynamic cube dropping onto a static platform).

## Decisions (locked with the user)

- **Scene model:** UI elements are **ECS entities** with UI components. Reuses the existing
  hierarchy, selection, inspector, undo, and TOML serialization. (Not a separate `.ui`
  asset, not an inline canvas node-tree.)
- **Text rendering:** **offline SDF atlas** baked in the `assetpack` tool via FreeType.
  Matches the SDF glyph path already written in the shaders. One font (Roboto) to start.
- **Editing surface:** **both** a dedicated 2D **Canvas editor panel** (editing scaffold)
  **and** a live **viewport overlay** (pixel-accurate preview via the real backend).
- **Anchoring:** full **Unity `RectTransform` model** - `anchorMin`/`anchorMax` (normalized)
  + `offsetMin`/`offsetMax` (px) + `pivot`. Anchor presets (9-point, stretch) are inspector
  shortcuts over this one representation.

### Scope assumptions (v1)

- **Render + author only.** No interactivity: no input, hit-testing, buttons, or hover.
- **New Scene loads a shipped `default.scene.toml` template** (data-driven, user-editable),
  not a hardcoded C++ builder. **Quick-save** writes `CaptureScene` to the *currently-loaded*
  scene name; New / Open / Save-As set that name (which the editor does not track today).
- **UI is editor-authored + engine-rendered.** C# scripting of UI is deferred. UI still
  fully serializes into the scene.

## Background: current architecture

- **GPU UI shaders already exist and are unwired** (no C++/C# references anywhere):
  - [`ui_shapes.slang`](../../../shaders/ui_shapes.slang) - one indirect-instanced draw
    emitting rounded rects, circles, lines, textured rects, and **SDF glyphs** from a
    device-addressable `DrawCommandData` buffer; push constant is
    `{ float4 screenSize, DevicePtr<DrawCommandData> commandData }`; bindless
    `g_textures[]` + `g_linearSampler` at **set 0**.
  - [`ui_build_draws.slang`](../../../shaders/ui_build_draws.slang) - compute preprocess:
    GPU bitonic sort of `DrawCommandData` by `layer` (single-workgroup path for
    N <= 256, multi-pass global path above), then a `DrawIndirectCommand` write pass.
  - [`UIStructs.slangh`](../../../shaders/include/UIStructs.slangh) - `DrawCommandData`
    (`data0`, `data1`, `color`, `type`, `layer`, `textureSlot`) and `DrawIndirectCommand`.
  - [`text_sdf.slangh`](../../../shaders/text_sdf.slangh) - `SdfGlyphAlpha()`, expecting an
    **R8_UNORM** single-channel SDF atlas (0.5 = glyph edge).
- **ECS** - [`Components.hpp`](../../../src/engine/scene/Components.hpp): plain-data structs,
  `World` owns storage; `HierarchyComponent` (parent + ordered children, mutated only via
  `ecs::SetParent`); `TransformComponent`, `NameComponent`.
- **Serialization** - [`SceneSerializer.hpp`](../../../src/app/scene/SceneSerializer.hpp):
  value-typed `SceneDescription` / `EntityRecord` <-> TOML. `EntityRecord` uses
  `std::optional<T>` per component (e.g. `physics`, `pointLight`). `CaptureScene`,
  `ApplyScene`, `ReplaceScene`, `SaveSceneFile`, `LoadSceneFile`, `ListSceneFiles`,
  `ScenesDirectory()`. Format version `kSceneFormatVersion = 5`.
- **Physics** - [`PhysicsComponents.hpp`](../../../src/engine/physics/PhysicsComponents.hpp):
  descriptor pattern. Emplace `BoxBodyDesc { halfExtents, motionType, layer, ... }` +
  `TransformComponent`; `PhysicsSystem::Update` creates the Jolt body and swaps in runtime
  components. Static platform = `Box` / `Static` / `NonMoving`; dynamic cube = `Box` /
  `Dynamic` / `Moving`.
- **Render graph** - [`RenderGraph.hpp`](../../../src/engine/rendering/RenderGraph.hpp):
  fluent `PassBuilder`. `AddComputePass` + `ReadWriteBuffer`/`ReadBuffer`/`WriteBuffer`;
  `AddPass` + `WriteColor(image, LoadOp, StoreOp, clear)` + `Execute(fn)`;
  `RegisterBuffer` / `UpdateExternalBuffer` for per-frame external device buffers;
  `EnsureBindlessSampled`. Precedent for device-address compute buffers: the skinning
  passes (`node_flatten`, `pose_init`, `skin_palette_build`).
- **Frame tail** - [`PostProcessStack.cpp`](../../../src/engine/passes/PostProcessStack.cpp):
  `$PostProcess` (tonemap) writes `m_ldrColor`; `$FXAA` reads `m_ldrColor` and writes
  `m_outputToTexture ? m_finalColor : GetSwapchainColor()`. In the **editor**,
  `m_finalColor` is the offscreen image the 3D viewport samples
  ([`ViewportPanel.cpp`](../../../src/app/debug/ViewportPanel.cpp) registers its view as an
  ImGui texture, `m_sceneViewportTextureId`, blitted via `AddImage`). In **standalone**,
  FXAA writes the swapchain directly.
- **Editor panels** - [`DebugPanel.hpp`](../../../src/app/debug/DebugPanel.hpp): base class
  (`GetName`, `OnImGui`, `OnUpdate`, `VisiblePtr`, `Load/SaveSettings`) owned by
  `DebugLayer`. [`ComponentDrawers`](../../../src/app/debug/ComponentDrawers.hpp) render the
  inspector. [`HierarchyPanel.cpp`](../../../src/app/debug/HierarchyPanel.cpp) has the
  create-entity menu (Cube/Sphere/Plane/lights) and an existing **Save-As** popup
  (`CaptureScene` + `SaveSceneFile(m_sceneNameBuf, ...)`). No quick-save / New Scene yet.
- **Fonts** - FreeType is linked into the engine; `assetpack` already links `stb_image` and
  has processor precedents ([`TextureProcessor`](../../../tools/assetpack/TextureProcessor.hpp),
  `MeshProcessor`, `PakWriter`). Binary asset conventions in
  [`BinaryFormats.hpp`](../../../include/BinaryFormats.hpp).

## Design

### 1. UI components (ECS)

New header `src/engine/ui/UiComponents.hpp`. UI parenting **reuses `HierarchyComponent`**; a
`UICanvas` entity roots a tree and anchoring resolves against the parent UI element's
resolved rect.

```cpp
struct UICanvas {                    // marks a UI-tree root; resolves to the output rect
    enum class ScaleMode : uint8_t { ConstantPixel, ScaleWithReference };
    ScaleMode scaleMode = ScaleMode::ConstantPixel;
    glm::vec2 referenceResolution{1920.f, 1080.f}; // used by ScaleWithReference
    int       sortBias = 0;                         // ordering when >1 canvas
};

struct UIRect {                      // Unity RectTransform
    glm::vec2 anchorMin{0.5f, 0.5f};
    glm::vec2 anchorMax{0.5f, 0.5f};
    glm::vec2 offsetMin{-50.f, -50.f}; // px; anchorMin==anchorMax => encodes pos/size
    glm::vec2 offsetMax{ 50.f,  50.f};
    glm::vec2 pivot{0.5f, 0.5f};
    glm::vec4 resolvedRect{0.f};       // x,y,w,h output px; recomputed per frame, NOT serialized
};

struct UIImage {                     // solid/rounded quad OR textured quad ("quad")
    glm::vec4     color{1.f};
    float         cornerRadius = 0.f;  // shapes shader already supports this
    TextureHandle texture{};           // invalid handle => solid color
};

struct UIText {                      // "text"
    std::string text;
    std::string fontName = "Roboto";
    float       pixelSize = 24.f;
    glm::vec4   color{1.f};
    enum class HAlign : uint8_t { Left, Center, Right };
    enum class VAlign : uint8_t { Top, Middle, Bottom };
    HAlign hAlign = HAlign::Left;
    VAlign vAlign = VAlign::Top;
    bool   wrap = true;                // wrap at rect width
};
```

`UIRect`+`UIImage` = quad; `UIRect`+`UIText` = text; both = labeled panel; `UICanvas`+`UIRect`
= root. `resolvedRect` is runtime-only (recomputed each frame, never serialized).

### 2. Layout + draw-command generation (CPU, per frame)

`src/engine/ui/UiLayoutSystem.*` walks each canvas subtree **pre-order** and resolves
`UIRect.resolvedRect` from the parent's resolved rect:

```
anchorRect.min = parent.min + anchorMin * parent.size
anchorRect.max = parent.min + anchorMax * parent.size
rect.min       = anchorRect.min + offsetMin
rect.max       = anchorRect.max + offsetMax          // size = max - min
```

Canvas-root rect = output extent (ConstantPixel) or reference-scaled (ScaleWithReference).

`src/engine/ui/UiDrawBuilder.*` then emits `DrawCommandData` in the **same pre-order**,
assigning `layer = running emission index`. This encodes painter order (parent behind child,
background behind its own text) directly in `layer`, so the existing GPU bitonic sort only
needs to make ordering deterministic - no stable-sort requirement. Mapping:

- `UIImage` with no texture -> `kShapeRect` (rounded when `cornerRadius > 0`).
- `UIImage` with texture -> `kShapeTexturedRect`, `textureSlot` = bindless slot; `data1` = full
  `(0,0,1,1)` UVs for v1.
- `UIText` -> shaping yields one `kShapeSdfGlyph` command per glyph: `data0` = glyph rect,
  `data1` = atlas UV rect, `textureSlot` = font atlas slot. Left-to-right advance/bearing from
  font metrics; `\n` breaks lines; optional word-wrap at rect width; H/V alignment offsets the
  line box within the rect.

### 3. Font pipeline (offline SDF)

New `tools/assetpack/FontProcessor.*`: FreeType rasterizes a codepoint set (ASCII + Latin-1 to
start) to SDF, packs into one **R8_UNORM** atlas (the format `text_sdf.slangh` expects), and
writes a **glyph metrics table**: per glyph `{ codepoint, atlas uv-rect, size px, bearing,
advance }` plus atlas dims and `{ ascent, descent, lineHeight }`. The atlas ships through the
existing texture/pak path; metrics ship as a small binary blob following
[`BinaryFormats.hpp`](../../../include/BinaryFormats.hpp) conventions.

Runtime `src/engine/ui/FontRegistry.*` + `FontAsset`: loads the atlas (-> bindless slot used as
`textureSlot`) and metrics; exposes lookup + a `ShapeText()` helper consumed by
`UiDrawBuilder`. One font baked to start; the format supports more, keyed by `UIText.fontName`.

### 4. UI renderer + render-graph integration

`src/engine/ui/UiRenderer.*` owns per-frame (triple-buffered) device-address buffers - a
`DrawCommandData` command buffer and a `DrawIndirectCommand` buffer - registered with the graph
via `RegisterBuffer` and refreshed with `UpdateExternalBuffer` (the per-frame external-buffer
pattern the skinning passes use). Each frame:

1. `UiDrawBuilder` fills the command buffer (host-visible or staged) with `N` commands.
2. **`$UiBuildDraws`** - `AddComputePass` + `ReadWriteBuffer(commands)` + `WriteBuffer(indirect)`,
   running `ui_build_draws.slang`. For the small `N` here the single-workgroup local-sort path is
   one dispatch; the indirect-write pass sets `instanceCount = N`.
3. **`$UiOverlay`** - `AddPass` appended **after `$FXAA`**, `WriteColor(output, LoadOp::Load,
   StoreOp::Store)` where `output` is the post-process stack's final image
   (`m_outputToTexture ? m_finalColor : GetSwapchainColor()` - exposed via a new
   `PostProcessStack` accessor). Alpha blending ON. `Execute` records `ui_shapes.slang` with
   `vkCmdDrawIndirect` (6 verts, `N` instances), push constant `{ screenSize, DevicePtr }`,
   bindless set 0.

Because `$UiOverlay` writes the exact image the editor viewport samples, the **viewport overlay
is the real, pixel-accurate preview for free** - both in-editor and in a standalone build.

### 5. Editor

- **`src/app/debug/UiCanvasPanel.*`** (new `DebugPanel`): a 2D editing scaffold - zoom/pan,
  rulers, canvas drawn at reference resolution, element bounding boxes, **anchor + pivot
  gizmos**, drag-move / drag-resize writing back to `UIRect`, snapping/guides. Drawn with ImGui
  draw lists; it does **not** host a second GPU renderer (the viewport overlay is the WYSIWYG
  surface). Selection is shared with the rest of the editor (`SceneSelection`).
- **Viewport overlay handles**: `ViewportPanel` draws selection/anchor handles over the live
  composited UI so elements can be nudged in-context; the pixels come from the backend.
- **Inspector**: `ComponentDrawers` gains drawers for `UICanvas`, `UIRect` (anchor-preset
  dropdown + Unity-style anchoredPosition/sizeDelta convenience view over the stored
  min/max), `UIImage`, `UIText`.
- **Hierarchy**: create menu gains **UI -> Canvas / Image / Text**; creating an Image/Text with
  no `UICanvas` ancestor auto-creates a canvas parent.

### 6. Serialization

Add optional records to `EntityRecord` (same `std::optional<T>` pattern as `physics`):
`uiCanvas`, `uiRect`, `uiImage`, `uiText`. `TextureHandle` re-resolves from its stable path;
`UIText.fontName` re-resolves via `FontRegistry` on apply. `resolvedRect` is never written. Bump
`kSceneFormatVersion` 5 -> 6; older files still parse (new fields simply absent).

### 7. Scene quick-save + New Scene

- **Current-scene tracking**: editor state gains the loaded/saved-as scene name; New / Open /
  Save-As set it.
- **Ctrl+S**: `CaptureScene` + `SaveSceneFile(currentName)`; if unnamed, open the existing
  Save-As popup.
- **New Scene**: `ReplaceScene` from a shipped `default.scene.toml` under `ScenesDirectory()` -
  a static `Box` platform + a dynamic `Box` cube above it (both physics descriptors) so the cube
  drops and lands. Data-driven and user-editable.
- **File menu** on the main menu bar (New / Open / Save / Save As) consolidates these entry
  points alongside the existing Save-As.

### 8. Testing

Headless doctests in the style of `SceneSerializerTests`:

- Anchor resolution: fixed anchors, stretch (anchors apart), nested subtrees, pivot.
- Text shaping: advance sums, word-wrap at width, H/V alignment offsets, newline handling.
- Draw-command emission: `layer` ordering (parent < child, background < glyphs), rect vs
  textured-rect vs glyph type/field mapping.
- Serialization round-trip for `UICanvas`/`UIRect`/`UIImage`/`UIText`; v5 file still loads.

Manual verification (per the `verify` workflow): run the editor, author a canvas with a quad +
text, confirm the viewport overlay matches the Canvas panel, save + reload, then New Scene and
watch the cube drop onto the platform.

## Non-goals (v1)

- No interactivity (input, hit-testing, buttons, hover, focus).
- No element rotation/scale - the shapes shader emits axis-aligned rects.
- No rich text, kerning, or multiple fonts within one string.
- No C# UI scripting API.
- No second offscreen GPU render for the Canvas panel (viewport is the pixel-accurate surface).

All are natural follow-ups on top of this foundation.
