# Camera Background — Unified 2D/3D Design

Date: 2026-07-17
Status: Approved for planning

## Problem

The scene background is currently a bolt-on to the procedural sky path, and it is
both colour-inaccurate and conceptually wrong for 2D scenes.

1. **Colour mismatch (WYSIWYG bug).** When a camera opts out of the sky
   (`CameraComponent::useSkyGradient == false`), its `clearColor` is copied into
   `skyHorizonColor` and emitted by `skybox.slang` into the **HDR** target
   (`AetherCore.cpp:670-678`, `skybox.slang:181-188`). That value then passes
   through the whole post stack — auto-exposure (luminance histogram) + ACES
   tonemap + sRGB encode. The authored colour is treated as *linear*, gets
   re-brightened/desaturated, and lands on screen visibly different from the
   ImGui swatch (which is drawn in display space). Picker and pixels live in two
   different colour spaces.

2. **Phantom sun gizmo in 2D.** `LightingPanel::AddLightGizmos`
   (`LightingPanel.cpp:130-140`) unconditionally draws a sun-direction line +
   sphere + ring from the renderer's **default** `m_sunDirectionIntensity`,
   ignoring scene kind and whether a directional light actually exists. A 2D
   scene with no sun still shows a sun arrow whenever light gizmos + debug
   rendering are on.

3. **No real background model.** Background is a single `bool useSkyGradient`.
   There is no solid-colour vs gradient choice, no multi-stop gradient, no angle
   — nothing a 2D or stylised-3D project needs.

## Goals

- A first-class **Background mode** on the camera: `SolidColour`, `Gradient`
  (multi-stop, angled), `SkyGradient` (the existing procedural sky).
- Solid and Gradient backgrounds are **WYSIWYG-exact**: the on-screen colour
  matches the authored swatch pixel-for-pixel, independent of tonemap operator
  and auto-exposure.
- `SkyGradient` behaviour is **unchanged** — existing 3D scenes render
  identically.
- Works identically for 2D and 3D scenes through **one** mechanism.
- The sun gizmo reflects the actual scene, not the renderer default.

## Non-goals

- No new procedural-sky features (clouds, scattering, moon) — the sky path is
  untouched beyond removing the flat-clear hack.
- No HDRI / cubemap / image backgrounds in this pass (the mode enum leaves room
  to add one later).
- No change to how DayNight drives the sky environment.

## Data model — `CameraComponent`

`useSkyGradient` is **removed** and replaced with an explicit mode plus gradient
authoring fields:

```cpp
enum class CameraBackground : std::uint8_t
{
    SolidColour = 0,
    Gradient    = 1,
    SkyGradient = 2,   // existing procedural sky (DayNight / renderer environment)
};

struct GradientStop
{
    glm::vec3 colour{0.0f};
    float     position = 0.0f;   // 0..1 along the gradient axis
};

// New fields on CameraComponent:
CameraBackground background = CameraBackground::SkyGradient; // 3D default
glm::vec3 clearColour{0.10f, 0.10f, 0.12f};                 // SolidColour mode
std::vector<GradientStop> gradientStops{                     // Gradient mode, >=2
    {{0.05f, 0.06f, 0.10f}, 0.0f},   // top
    {{0.02f, 0.02f, 0.03f}, 1.0f},   // bottom
};
float gradientAngleDegrees = 0.0f;                          // 0 = top->bottom
```

Naming note: the existing field is spelled `clearColor` (US). Keep the existing
spelling for the solid field to avoid churn in reflection/serializer/tests; the
struct field stays `clearColor`. (This section uses `clearColour` illustratively
only.)

**Defaults by scene kind:** new 3D cameras default to `SkyGradient`; new 2D
cameras default to `SolidColour`. This is set where cameras are seeded per scene
kind (not by changing the struct default, which stays `SkyGradient`).

**Ordering + validity:** `gradientStops` is kept sorted by `position` at author
time and at load. Positions are clamped to `[0,1]`. A mode-`Gradient` camera
with fewer than 2 stops is treated as `SolidColour` using the first stop (or the
`clearColor` if none), so the renderer never divides by an empty range.

## Render architecture — WYSIWYG composite at tonemap

The background is composited in **display space at the tonemap pass**, using the
**HDR target's alpha channel as a scene-coverage mask**. This is the only place
in the frame where linear→display happens, so it is the correct and only place a
pixel-exact background can be produced.

### Coverage convention (new invariant)

- The HDR colour target is cleared to `(0, 0, 0, 0)`. **Alpha 0 == "no scene
  drawn here" (background).**
- Opaque 3D forward (`$EngineForward`) writes **alpha = 1**.
- `$Skybox` in `SkyGradient` mode writes **alpha = 1** (it *is* the scene
  background and must remain HDR/tonemapped exactly as today).
- In `SolidColour` / `Gradient` modes, the `$Skybox` pass is **skipped**, so
  background pixels retain alpha 0.
- The 2D sprite pass accumulates coverage into alpha using a **separate
  over-operator alpha blend** (`alphaDst = srcA + alphaDst·(1-srcA)`), so
  anti-aliased / semi-transparent sprite edges yield fractional coverage and
  blend correctly against the background.

### Tonemap pass becomes the compositor

```
scene   = tonemap(hdr.rgb * exposure)          // existing path
bg      = EvalBackgroundDisplaySpace(pixel)    // solid or multi-stop gradient
final   = lerp(bg, scene, hdr.a)               // a=1 scene, a=0 background
```

`EvalBackgroundDisplaySpace` is evaluated **directly in display space** (the same
space the ImGui swatch is authored in), so Solid/Gradient match the picker
exactly and are immune to tonemap operator choice and auto-exposure.

Consequences (all desirable):

- A flat/solid background no longer contributes to bloom or drags
  auto-exposure — it is not in the HDR buffer at all in WYSIWYG modes.
- Sprites, particles, and 3D geometry still bloom and tonemap normally.
- `SkyGradient` mode is byte-for-byte unchanged: skybox writes HDR + alpha 1, so
  `lerp(bg, scene, 1) == scene`, and `EvalBackground` is never visible.

### Background params delivery

Background parameters are **not** added to `FrameConstants` (a locked 768-byte
layout with per-field offset asserts). Instead they ride to the tonemap pass via
a small **BDA-addressed buffer** referenced from `TonemapPush`:

```cpp
struct GpuBackgroundParams          // std140-friendly packing
{
    uint   mode;                    // 0 solid, 1 gradient (2 sky => composite disabled)
    uint   stopCount;               // number of valid stops (>=2 for gradient)
    float  angleRadians;            // gradient axis angle
    uint   _pad;
    // stops as (rgb, position) pairs; fixed capacity kMaxBackgroundStops (e.g. 8)
    glm::vec4 stops[kMaxBackgroundStops]; // xyz = display-space colour, w = position
};
```

`TonemapPush` gains a `uint64_t backgroundParamsAddr` (0 => composite disabled,
i.e. SkyGradient / no camera). This matches the engine's existing BDA idiom
(material buffer, shadow light data) and avoids push-constant size limits while
supporting an arbitrary (capped) stop count.

`kMaxBackgroundStops` is a fixed capacity (proposed 8). Author-time UI allows up
to that many; extra stops beyond capacity are rejected in the editor, not
silently dropped at extraction.

### Extraction / frame packet

`RenderFramePacket` carries the resolved background state (mode, angle, sorted
stops) captured on the game thread from the main camera — following the existing
"render thread reads only the packet, never the ECS" invariant
(`render-frame-extraction.md`). The tonemap-side BDA buffer is filled on the
render thread from that packet field each frame (same pattern as other per-frame
BDA buffers). The current `skyVoidColor.w == 0` flat-clear signalling is removed.

## Shader changes

- **`tonemap.slang`**: read `hdr.a`; add `EvalBackground()` that evaluates a
  solid colour or interpolates the multi-stop gradient in display space along the
  authored angle; composite `lerp(bg, tonemap(hdr), a)`. Guard on
  `backgroundParamsAddr != 0` (SkyGradient / no-camera => pass-through, current
  behaviour).
- **`skybox.slang`**: delete the `skyVoidColor.w < 0.5` flat-clear branch
  (lines 181-188). Ensure the sky fragment outputs alpha 1.
- **Forward mesh + 2D sprite pipelines**: HDR clear alpha 0; opaque forward
  outputs alpha 1; 2D sprite pipeline uses a **separate** alpha blend
  (over-operator on the alpha channel) so coverage accumulates. This blend-state
  change is the primary implementation risk and gets an explicit verification
  step (below).

Shader edits require the `App_CompileShaders` target + `EngineAssetsPak` rebuild
(per project conventions).

## Sun gizmo fix

`LightingPanel::AddLightGizmos` only draws the sun-direction gizmo when **both**:

1. the scene is 3D (`SceneKind` / scene features indicate 3D meshes/lighting),
   and
2. the scene actually contains a directional source — a live `DayNightComponent`
   driver **or** an explicit directional light — rather than the renderer's
   default `m_sunDirectionIntensity`.

The point/spot gizmos already iterate real lights; only the sun branch needs the
guard. No sun in the scene => no sun gizmo, in any scene kind.

## Reflection / serialization / editor UI

- **Reflection** (`CoreComponents.reflect.cpp`): reflect the `background` enum
  and the `gradientStops` list + `gradientAngleDegrees`, replacing the
  `use_sky_gradient` field. The variable-length stop list is the heaviest new
  surface: it needs list (de)serialization and a list editor.
- **Serialization** (`SceneSerializer*`): write the new keys
  (`background`, `gradient_stops`, `gradient_angle`, `clear_color`). **Back-compat
  read**: a scene with legacy `use_sky_gradient` (and no `background` key) maps
  `true -> SkyGradient`, `false -> SolidColour`, preserving `clear_color`. Stops
  serialize as an array of `{ colour = [r,g,b], position = p }` tables.
- **Editor UI** (`ComponentDrawers.cpp`): replace the current
  background-radio + colour picker with:
  - a **mode dropdown** (Solid Colour / Gradient / Sky Gradient),
  - Solid: a colour picker (existing `PropColor3` on `clearColor`),
  - Gradient: a **stop list editor** — per-stop colour swatch + position slider,
    add / remove / reorder, kept sorted; plus an **angle** control,
  - Sky Gradient: no extra controls (environment owns it).

## Testing

- **Unit (SceneSerializerTests):**
  - round-trip a camera in each mode incl. a >=3-stop gradient with a non-zero
    angle;
  - legacy migration: `use_sky_gradient = false` -> `SolidColour` with preserved
    `clear_color`; `= true` -> `SkyGradient`;
  - stop clamping/sorting on load.
- **Runtime verification (aethercore MCP):**
  - author a 2D scene, set `SolidColour` to a known value, screenshot, and sample
    the centre background pixel ≈ authored colour (the actual WYSIWYG assertion,
    within a small display-space tolerance);
  - author a vertical gradient, sample top vs bottom pixels match the end stops;
  - confirm a 3D `SkyGradient` scene renders unchanged (spot-check pixels
    against a pre-change capture);
  - confirm the sun gizmo is absent in a 2D scene with light gizmos enabled and
    present in a 3D scene with a DayNight driver.
- **Coverage sanity:** a 3D scene with `SolidColour` — geometry tonemapped over
  an exact background — verifies alpha coverage works with real depth-tested
  opaque draws, and 2D sprites over a gradient verify fractional-edge coverage.

## Risks

- **Alpha-coverage convention across passes** is the main risk: the HDR clear
  alpha and the forward/2D blend-state changes must be verified on real frames
  (3D-over-sky unchanged, 3D-over-solid, 2D-sprites-over-gradient). Mitigation:
  explicit runtime verification step before claiming done; `SkyGradient` path is
  provably unchanged (alpha 1 everywhere) so existing scenes are the safety net.
- **Stop-list plumbing** (reflection + serializer + inspector list UI) is the
  largest net-new surface; it is isolated from the render architecture and can be
  built/tested independently.
- **Back-compat**: every existing scene file uses `use_sky_gradient` — migration
  read must be covered by a test before touching the serializer write path.
```
