# MCP: Launcher Methods + ImGui UI Automation — Design

Date: 2026-07-17
Status: Approved for planning

## Problem

Two related gaps in the MCP control surface:

1. **The launcher's new recents operations aren't exposed.** `remove_recent`,
   `reveal_folder`, and `relocate_recent` exist as `LauncherLayer` methods (and UI
   affordances) but have no control-method equivalent, so nothing can drive or
   verify them headlessly.

2. **No way to drive or read the ImGui UI.** The editor/launcher UI is ImGui, but
   the MCP can only take pixel screenshots — it cannot click a widget, hover to
   reveal hover-only UI (the recents card ×, tooltips, context menus), or query
   what widgets are on screen and where. This blocks accurate UI navigation and
   left the launcher's interactive elements only build-verified, never driven.

## Goals

- Expose the launcher recents operations as control methods.
- Add an ImGui automation layer: query on-screen widgets (label, window, rect,
  state) and simulate mouse/keyboard input to navigate the UI accurately.
- Share the automation methods across the editor and launcher control endpoints.
- Keep the main-thread dispatch model (no new MCP worker thread — see
  `aethercore-mcp-threading` memory).

## Non-goals

- No full Dear ImGui Test Engine dependency (custom thin layer, per decision).
- No pixel-level image analysis / OCR — queries are structural (widget registry).
- No change to the ControlServer threading model; blocking-handler async cleanup
  is a separate future item (noted in memory), not in scope here.
- No general record/replay test framework — just on-demand query + input.

---

## Part A — Launcher control methods

Three methods added to `BuildLauncherControlMethods` (`LauncherLayer.cpp`),
mirroring the existing `open_project`/`create_project` shape. Handlers run on the
main thread (via `DrainCommands`), so they call `LauncherLayer` methods directly.

- `launcher.remove_recent` (mcp `remove_recent`, mutating) — params `{root}`
  (required). Calls `RemoveRecent(root)`. Returns
  `{removed: bool, projects: [{name, root}, ...]}` (updated list).
- `launcher.reveal_folder` (mcp `reveal_folder`, non-mutating) — params `{root}`
  (required). Calls `RevealProjectFolder(root)`. Returns `{status: "opened", root}`.
- `launcher.relocate_recent` (mcp `relocate_recent`, mutating) — params
  `{old_root, new_root}` (both required). Calls a new headless overload
  `RelocateRecent(oldRoot, newRoot, std::string& error)`; returns
  `{projects: [...]}` on success or `{error}` on failure.

**Refactor:** extract the relocate core into
`bool LauncherLayer::RelocateRecent(const std::filesystem::path& oldRoot, const std::filesystem::path& newRoot, std::string& error)`
(validates `newRoot` has a descriptor via `HasProjectDescriptor`, then
`RemoveRecent(old)` + `RememberRecent(new)`). The existing UI
`RelocateRecent(oldRoot)` opens the folder picker, then delegates to it.

`tools/mcp/manifest.json` is regenerated (it reflects the live method table) and
committed.

---

## Part B — ImGui UI automation

### Module

New singleton `aether::app::UiAutomation` in `src/app/imgui/UiAutomation.{hpp,cpp}`.
A singleton (not a service) because the ImGui hooks are global C-linkage
functions; `UiAutomation::Get()` returns the instance. It owns:

1. a **per-frame item registry** (built via the ImGui test-engine hooks), and
2. a **synthetic input queue + state machine** (applied each frame before
   `ImGui::NewFrame`).

### Item registry (query)

Enable `IMGUI_ENABLE_TEST_ENGINE` on the `imgui` CMake target. Implement the four
extern hook symbols (declared in `imgui_internal.h:4294`) in `UiAutomation.cpp`:

```cpp
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* item_data); // item_data may be NULL
void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags);
void ImGuiTestEngineHook_Log(ImGuiContext* ctx, const char* fmt, ...);          // no-op
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext* ctx, ImGuiID id);   // returns stored label or ""
```

`ItemAdd` records `{id, rect = bb (screen space), window = ctx->CurrentWindow->Name}`
into a **building** list; `ItemInfo` fills in the `label` for that id. Gating flag
`ctx->TestEngineHookItems` must be set true once after the ImGui context is
created (in `ImguiSubsystem` init).

Registry entry:

```cpp
struct UiItem { ImGuiID id; std::string label; std::string window; float x, y, w, h; };
```

Double-buffered: at `BeginFrame`, swap building -> current snapshot and clear
building; widgets fill building during the frame. MCP queries read the **current
snapshot** (last completed frame) — stable because `DrainCommands` runs on the
main thread between frames. Hovered/active are computed at query time by comparing
`id` to `GImGui->HoveredId` / `GImGui->ActiveId` (not stored per frame).

### Synthetic input (actions)

`ImguiSubsystem::BeginFrame` calls `UiAutomation::Get().ApplyInput()` **after**
`ImGui_ImplGlfw_NewFrame()` and **before** `ImGui::NewFrame()`
(`ImguiSubsystem.cpp:196-199`) — so injected events override the real cursor for
that frame. Injection uses ImGui's IO event queue:
`io.AddMousePosEvent`, `io.AddMouseButtonEvent`, `io.AddKeyEvent`,
`io.AddInputCharactersUTF8`.

Actions are queued by MCP handlers and played by a per-frame state machine:

- **Click** (`{x,y}` or resolved item center, button, single/double): frame 1 sets
  mouse pos (establishes hover); frame 2 pos + button-down (covers press-triggered
  widgets like menus and the recents card × which uses `IsMouseClicked`); frame 3
  button-up (covers release-triggered widgets like buttons). Double-click replays
  two down/up pairs within `io.MouseDoubleClickTime`.
- **Hover** (`{x,y}` or item): a **held** mouse position re-asserted every frame
  (so it survives GLFW re-asserting the real cursor) until the next action changes
  or clears it — this reveals hover-only UI (card ×, tooltips) and lets a
  subsequent click land on a context-menu item.
- **Input text** (item or `{x,y}`, text): click to focus, then Ctrl+A + Delete to
  clear, then `AddInputCharactersUTF8(text)`.
- **Key** (named key: enter/escape/tab/…): a down+up `AddKeyEvent` pair, mapped
  from a small name->`ImGuiKey` table.

### Async model

An action cannot complete inside one handler call — it needs multiple frames, and
the handler runs on the loop thread (blocking would deadlock the frame advance
that the action depends on). So action methods **enqueue and return immediately**
(`{status: "queued"}`). The ~3 frames play out in milliseconds; by the agent's
next tool call (query/screenshot, hundreds of ms later) the effect is visible.
Multi-step flows (right-click -> read menu -> click item) are done as separate
tool calls, which the async model handles naturally.

### MCP surface (`ui.*`, shared by editor + launcher)

A shared `BuildUiAutomationControlMethods()` appended to both the editor method
table and `BuildLauncherControlMethods`.

- `ui.query` (mcp `ui_query`, non-mutating) — optional `{window, label}` substring
  filters. Returns `{items: [{window, label, x, y, w, h, hovered, active}]}` from
  the current snapshot.
- `ui.click` (mcp `ui_click`, mutating) — `{x, y}` OR `{window, label}`; optional
  `button` ("left"/"right", default left), `double` (bool). Resolves an item to
  its rect center; errors if a `{window,label}` target is not found. Returns
  `{status: "queued", target}`.
- `ui.hover` (mcp `ui_hover`, mutating) — `{x, y}` OR `{window, label}`; holds the
  cursor there. Returns `{status: "queued"}`.
- `ui.input_text` (mcp `ui_input_text`, mutating) — target + `{text}`. Returns
  `{status: "queued"}`.
- `ui.key` (mcp `ui_key`, mutating) — `{key}` name. Returns `{status: "queued"}`.

Item resolution for `{window,label}`: exact label match within the window if
given, else first label match across windows; ambiguous/none -> error listing
candidates.

---

## Testing

### Unit (doctest, no GPU)
- **Input state machine**: a headless `InputScript` that, given a queued click,
  emits the correct ordered sequence of synthetic events across `Step()` calls
  (pos; pos+down; up) and reports completion. Test click, double-click, hover-hold
  (re-asserts pos every step), key (down/up), input-text (clear then chars).
- **Registry lookup**: given a set of `UiItem`s, `FindItem(window,label)` returns
  the right one, handles ambiguity (error) and miss (error), and centre-point math
  is correct.
- **Launcher relocate core**: `ComposeNewProjectRoot` already covered; add a test
  that the relocate validation path rejects a `new_root` lacking a descriptor
  (using a temp dir), if `LauncherLayer` is constructible in isolation; otherwise
  cover the pure validation helper it delegates to.

### Runtime (aethercore MCP, live launcher/editor) — the payoff
- Launch the launcher with a seeded missing recent; `ui_query` to find the card,
  `ui_hover` it, `ui_click` its × coordinates, then `remove_recent` cross-check
  and `list_projects` to confirm the entry is gone.
- `ui_click` (right) a card, `ui_query` the context menu, `ui_click` "Remove from
  list", confirm removal.
- Open the Create dialog via `ui_click` on "New Project", `ui_query` to read the
  Name/Location fields and the Create button's disabled state, `ui_input_text` a
  name + location, screenshot the live `Creates:` preview.
- `ui_query` a 3D editor scene to confirm items from a docked panel are reported
  with sane rects.

---

## Risks

- **Hook signature drift**: the four extern symbols must match the vendored
  `imgui_internal.h` (v1.92.8-docking) exactly — verified at design time
  (`imgui_internal.h:4294-4298`); re-check if imgui is bumped. A mismatch is a
  link error, caught at build.
- **`IMGUI_ENABLE_TEST_ENGINE` scope**: the define must be set when compiling
  imgui's own TUs (so imgui.cpp emits the hook calls) AND visible to
  `UiAutomation.cpp` (for the `ImRect`/`ImGuiLastItemData` types) — set it as a
  PUBLIC define on the `imgui` target. One-time full imgui rebuild.
- **Injected pos vs. real cursor**: injection must run after
  `ImGui_ImplGlfw_NewFrame` so our event is later in the queue and wins for the
  frame; hover-hold must re-assert every frame.
- **Widget trigger timing**: press- vs release-triggered widgets need the 3-frame
  click; the recents card × (press) and standard buttons (release) are both
  covered by design and verified at runtime.
- **Overhead**: hooks fire for every item every frame; negligible for an editor,
  but the registry building list is cleared and swapped (no per-frame allocation
  churn beyond vector reuse).
