# UI Text Box + Clipping — Design

Date: 2026-07-27
Status: Approved

## Motivation

The UI system has no text entry. The immediate consumer is an ENet multiplayer
connect screen that needs an IP:port field, but the widget outlives that use
(player name, chat, search, save-slot rename).

Scope decision: build a **full text field** — caret movement, shift/drag
selection, clipboard, horizontal scroll — not an append-only stub. A scrolling
field needs clipping, and clipping is built as a **general UI mechanism**
(`UIMask`, Unity's RectMask2D) rather than a textbox-private trick, because that
is what makes the investment pay for scroll views and list viewports later.

## Current system

- Components live in `src/engine/ui/UiComponents.hpp`: `UICanvas`, `UIRect`,
  `UIImage`, `UIText`, `UISelectable`, `UISlider`, `UIToggle`, `UIButton`,
  `UIProgressBar`, `UIEffect`, `UIMaterial`. Widgets self-draw and carry their
  own label/colours; they do not compose child text entities.
- `UiNavigationSystem` owns focus and activation across `UISelectable`s (spatial
  keyboard nav + mouse hover/click). `UiWidgetSystem` drives widget values.
  Both are ticked from `src/app/systems/ScriptComponentSystem.cpp:223-224`,
  navigation **first**.
- `UiDrawBuilder::Walk` recurses the canvas tree emitting `UiDrawCommand`s;
  `UiRenderer` memcpys them into a device-addressable buffer, `ui_build_draws.slang`
  sorts them by layer, `ui_shapes.slang` draws them in one indirect instanced draw.
- One `AE_COMPONENT` block in `src/app/scene/reflection/MoreComponents.reflect.cpp`
  drives MCP, the component palette, the inspector and serialization.
- `Input::GetTypedChars()` already exists (GLFW char callback — layout, dead-key
  and IME correct) and has **zero callers**. The textbox is its first consumer.

## Design

Three separable pieces, in dependency order.

### 1. Clipping infrastructure

`UiDrawCommand` gains a clip rect and a flag:

```cpp
inline constexpr std::uint32_t kFlagClip = 1u << 2;

struct UiDrawCommand
{
    glm::vec4 data0, data1, color;
    glm::vec4 clipRect{0.f};        // (x, y, w, h) px; honoured only when kFlagClip is set
    std::uint32_t type;             // field order is load-bearing: must stay
    std::int32_t  layer;            // byte-for-byte with DrawCommandData's std430 layout
    std::uint32_t textureSlot;
    std::uint32_t flags;
};
static_assert(sizeof(UiDrawCommand) == 80);
```

A flag rather than a sentinel, so a zero-size clip legitimately hides content
instead of reading as "unclipped".

- `DrawCommandData` in `src/shaders/include/UIStructs.slangh` gains the matching
  `float4 clipRect`. Upload is a straight memcpy of the vector
  (`UiRenderer.cpp:546`), so the size change is transparent to the C++ side.
- `ui_build_draws.slang`'s groupshared bitonic sort array grows 256×64 = 16 KB to
  256×80 = 20 KB, under the 32 KB floor. No logic change; add a comment recording
  the headroom so a future field growth is a considered decision.
- The clip test happens in the **fragment** stage. `SV_Position.xy` there is
  already window pixel coordinates, the same space UI rects are authored in, so
  the test is a two-line `discard` needing one `nointerpolation float4` varying.
  Fragment-side (not vertex-side quad shrinking) because it is uniformly correct
  for circles, lines and corner-radius antialiasing, which geometric shrinking
  would distort.

**Shared `VSOutput` header.** Project material shaders (INKBOUND's
`ui_glitch_text`, `ui_dialogue_text`, `ui_ink_ui`, …) pair with the engine's
shared UI *vertex* shader and hand-copy its `VSOutput` struct, which they are
required to match exactly. Adding a varying silently breaks them. So `VSOutput`
moves into a new `src/shaders/include/UiVertexOut.slangh` alongside a
`UiClipDiscard(input)` helper; `ui_shapes.slang` includes it, and the project
shaders are migrated from their copies to the include. This converts a silent
breakage trap into a compile-time contract — a one-time migration that pays off
on every future varying.

**Clip stack in the builder.** `UiDrawBuilder::Walk` carries a current clip rect
down the tree. A `UIMask { bool enabled; float padding; }` intersects its own
padded `UIRect` into the clip for its entire subtree; emitted commands are
stamped with the active clip. The textbox stamps its own text/caret/selection
commands with its padded inner rect regardless of any ancestor mask.

### 2. Components

```cpp
struct UITextBox
{
    enum class ContentType : std::uint8_t { Any, Integer, Decimal, Alphanumeric, Host };

    std::string text, placeholder, fontName = "Roboto";
    std::string allowedChars;       // escape hatch: non-empty = whitelist, ANDed with contentType
    ContentType contentType = ContentType::Any;
    int   maxLength = 0;            // 0 = unlimited
    bool  password  = false;        // render as '*'
    float pixelSize = 20.f, cornerRadius = 4.f, padding = 8.f;
    glm::vec4 bgColor, bgColorFocused, textColor, placeholderColor, caretColor, selectionColor;

    // runtime, never authored
    bool  editing = false, changed = false, submitted = false, cancelled = false, dragging = false;
    int   caret = 0, selectionAnchor = 0;   // byte indices into `text`
    float scrollX = 0.f, caretTimer = 0.f, repeatTimer = 0.f;
};

struct UIMask { bool enabled = true; float padding = 0.f; };
struct UIKeyboardCapture {};   // marker: the focused owner consumes navigation keys
```

Both `contentType` and `allowedChars` ship: the enum is authorable from the
inspector and MCP with no script, the raw charset covers what the enum does not.
They are ANDed — a character must pass both.

`UIKeyboardCapture` is the only coupling between navigation and text editing.
The textbox system adds and removes it; `UiNavigationSystem` never mentions
`UITextBox`, so a future dropdown or spinner claims keys the same way.

### 3. Editing core and system

All editing logic lives in `src/engine/ui/UiTextEdit.{hpp,cpp}` as **pure free
functions** over a small state struct — no `World`, no `Input`, no GLFW. This
mirrors the existing idiom (`SliderQuantize`, `SliderValueFromMouseX` in
`UiWidgetSystem.hpp`), which is what makes `tests/ui/UiWidgetSystemTests.cpp`
runnable headless.

Functions: `FilterInsert` (contentType + allowedChars + maxLength),
`InsertText`, `DeleteSelection`, `DeleteBackward`, `DeleteForward`,
`MoveCaret(dir, shift, ctrl)`, `WordBoundary`, `SelectAll`,
`CaretFromMouseX`, `CaretToPixelX`, `ScrollToCaret`.

`src/engine/ui/UiTextBoxSystem.{hpp,cpp}` is a thin marshaller: ECS + `Input`
into those pure calls. Its own translation unit rather than an addition to
`UiWidgetSystem.cpp` — that file is 154 lines and this is roughly 300. It is
ticked from `ScriptComponentSystem.cpp` after navigation, beside the existing
`UiWidgetSystem::Update` call.

Key repeat (backspace, arrows) is owned by the system: a 0.4 s initial delay and
0.03 s rate driven off held keys, rather than adding repeat state to `Input`.

### 4. Focus and capture contract

`UiNavigationSystem` runs before the widget systems, so it observes the previous
frame's capture state. That ordering makes every transition fall out correctly:

- Click, or Enter/Space on a focused textbox: navigation sets `activated`, the
  textbox enters editing and gains `UIKeyboardCapture`.
- The following frame navigation sees the focused element has capture and skips
  Left/Right/Up/Down/Enter/Space entirely. Caret keys and Enter reach only the
  textbox.
- Enter or Escape while editing: the textbox drops capture and sets `submitted`
  or `cancelled`. Navigation had already yielded that frame, so the same Enter
  never also activates a neighbour.
- **Tab** is new in navigation: it exits editing and moves focus to the next
  selectable in reading order (top-to-bottom, then left-to-right by resolved
  rect). A connect form needs it — IP field, port field, Connect button.
- Click outside: navigation refocuses elsewhere, the textbox observes it lost
  focus, commits, and drops capture.

Mouse editing: click places the caret, drag extends the selection, double-click
selects the word under the cursor, Ctrl+A selects all.

### 5. Input additions

- `SetSyntheticChars(std::string)`, mirroring `SetSyntheticKey` and
  `SetSyntheticMouse`; consumed and cleared each `Update()`. MCP `send_input`
  gains a `text` field so a `.seq` playtest can type an IP and press Enter.
- `GetClipboardText()` / `SetClipboardText()` over `glfwGet/SetClipboardString`,
  wired to Ctrl+C/X/V and exposed to C# as `Input.Clipboard`. With no window
  (unit tests) both fall back to an internal string — that is the test seam for
  paste.

### 6. Authoring and scripting surface

- Two `AE_COMPONENT` blocks in `MoreComponents.reflect.cpp`:
  `UiTextBoxComponent` ("UI Text Box", `ICON_FA_KEYBOARD`) and `UiMaskComponent`,
  the first with `PostSet -> EnsureWidgetCompanions(w, e, true)` to attach
  `UIRect` + `UISelectable`. One declaration each drives MCP, the palette, the
  inspector and serialization (`AE_GENERIC_SERIALIZE`).
- `CreateTextBoxEntity` in `UiEntities.{hpp,cpp}`.
- `UiExports.cpp` + `Ui.cs`: `CreateTextBox`, `GetTextBoxText`, `SetTextBoxText`,
  `WasSubmitted`, `WasCancelled`, `IsEditing`, `BeginEdit`, `SetPlaceholder`,
  `SetContentType`. The existing `WasChanged` generalizes to the textbox.
- Editor summaries in `ComponentDrawers.cpp` and `UiCanvasPanel.cpp`, following
  the existing slider/toggle lines.

## Testing

- `tests/ui/UiTextEditTests.cpp` — the pure core: filtering per content type,
  `allowedChars` intersection, `maxLength`, caret and word movement,
  shift-selection, typing over a selection, backspace/delete at string
  boundaries, clipboard round-trip through the no-window seam, scroll follows
  caret, click-to-caret mapping.
- `tests/ui/UiDrawBuilderTests.cpp` — clip stamping, `UIMask` subtree
  inheritance, nested mask intersection, textbox self-clip under a mask.
- Navigation — a captured focus yields arrow and Enter keys; Tab advances in
  reading order.
- Headless end-to-end: an input sequence types an address into a textbox and
  submits, via the new synthetic-chars route.

Shader changes require the `App_CompileShaders` target and an `EngineAssetsPak`
rebuild before they take effect.

## Deliberate limitations

- **ASCII only.** `ShapeText` treats each `char` byte as a codepoint
  (`FontRegistry.cpp:58`) and the baked atlases are ASCII, so multibyte UTF-8
  would render as mojibake rather than simply fail. Insertion filters to
  printable ASCII `0x20`–`0x7E`, and caret indices are therefore byte == char.
  Widening this is a font-pipeline change, not a textbox change.
- **Single line.** No multiline, no word wrap inside the field.
- **No IME preedit composition.** GLFW's char callback delivers committed
  characters; an in-progress composition string is not rendered.
- **No right-to-left or bidi.**

## Non-goals

Undo/redo inside the field, rich text, autocomplete, and the ENet networking
work itself. This spec covers only the input widget and the clipping mechanism
it needs.
