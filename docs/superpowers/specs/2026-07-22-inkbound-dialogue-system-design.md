# INKBOUND Dialogue System — Design

**Date:** 2026-07-22
**Status:** Approved (brainstorming complete; ready for implementation plan)

## Goal

A branching, data-driven dialogue system for INKBOUND that matches the game's dark/eerie
ink menu theme. Conversations are authored as external JSON files, can branch on player
choices, and are triggered three ways: walking into a zone, pressing interact near a
speaker/object, or a script call. An active dialogue freezes gameplay.

## Guiding constraints

- **Project boundary (hard rule):** all game logic lives in `projects/INKBOUND/` —
  scripts + `assets/dialogue/*.json` + reuse of existing UI shaders. **No** dialogue-specific
  code in `src/engine`, `src/app`, or `managed/AetherCore` (the SDK).
- **The single allowed engine addition** is a *generic* asset-text read (`Assets.ReadText`),
  justified because any game needs to read data assets — it is not dialogue-specific.
- **No shortcuts** — proper architecture, not timer/dirty-flag hacks.
- Reuse what the menu suite already built: `Ui.*` API, `UIButton` focus/nav, the `▸` ink
  focus marker, IBM Plex Mono Italic + PixelStorm fonts, the cyan `GameSettings.Accent`,
  and the ink shader palette.

## Architecture overview

```
                 assets/dialogue/<id>.json   (author-facing content)
                            │  Assets.ReadText (generic engine hook)
                            ▼
   DialogueGraph  ◄── JSON parse (System.Text.Json)        [pure C#, unit-testable]
        │
        ▼
   DialogueRunner (EntityScript, persistent)               [runtime + presentation]
   ├─ Time.Pause while active; animates on Time.UnscaledTime
   ├─ box UI (ink panel, IBM Plex Mono text, PixelStorm speaker)
   ├─ typewriter reveal + advance/skip
   └─ choices via existing UIButton focus/nav + condition eval

   Dialogue (static facade)   Play("id") / IsActive / SetFlag / HasFlag
   DialogueState              flags(set<string>) + ints; condition eval; effects
   DialogueTrigger (EntityScript)  zone | interact-key | (script call is just Dialogue.Play)
```

## 1 · Engine addition — generic asset-text read

Exactly one change outside the project. The engine already has
`FileSystem::ReadFileText(virtualPath) -> Expected<std::string>` (resolves through the VFS:
raw project dir in-editor, `.pak` when shipped).

- **Interop** (`src/app/scripting/interop/AssetsExports.cpp`, new TU — remember `cmake -S/-B`
  reconfigure so the Editor target picks it up, per the new-interop-cpp gotcha):
  ```cpp
  // Reads a virtual asset path as UTF-8 text into caller buffer; returns byte length
  // (or the needed length if the buffer is too small; 0/-1 on missing/error).
  AE_SCRIPT_API std::int32_t aether_assets_read_text(const char* vpath, char* out, std::int32_t cap);
  ```
  Runtime-safe: `io/FileSystem.hpp` only, no editor/ImGui deps.
- **Managed** `managed/AetherCore/Assets.cs` (new SDK file — generic, not INKBOUND):
  ```csharp
  public static class Assets
  {
      /// <summary>Read a data asset as UTF-8 text via the VFS (raw dir in-editor, pak when shipped).
      /// Returns null if the asset does not exist.</summary>
      public static string? ReadText(string virtualPath);
  }
  ```
  Two-call pattern (query length, then fill) like `Ui.GetText`/`Ui.GetButtonLabel`.
- **Path convention:** dialogue files resolve under the project mount, e.g.
  `Assets.ReadText("dialogue/intro.json")`. Confirm the actual VFS prefix against how the
  project mounts assets during the plan (may be `project://dialogue/intro.json`).

## 2 · Data format — JSON

Location: `projects/INKBOUND/assets/dialogue/<id>.json`.

```jsonc
{
  "id": "intro",
  "start": "s0",
  "nodes": {
    "s0": {
      "speaker": "The Void",
      "text": "You should not have come this far down.",
      "choices": [
        { "text": "Keep descending.", "goto": "deeper", "set": "chose_descend" },
        { "text": "Turn back.",       "goto": "warned",  "if": "coins>=3" }
      ]
    },
    "deeper": { "speaker": "The Void", "text": "Then bring your ink. You will need it.", "goto": "end" }
  }
}
```

Rules:
- A **node** has `speaker` + `text`, and **either** a linear `goto` **or** a `choices` array.
  A node with neither (or `goto: "end"` / a missing target) **ends** the conversation.
- A **choice** has `text` + `goto`, plus optional `if` (condition) and `set` (effect flag).
- Choices whose `if` evaluates false are **hidden** (not shown greyed).
- **Text is ASCII-only** (font atlas limitation, per the INKBOUND UI gotcha).

### Condition / effect grammar (minimal, extensible)

Evaluated by `DialogueState`. Deliberately tiny — grow only when a real line needs it.

| Form           | Meaning                                   |
|----------------|-------------------------------------------|
| `flagName`     | flag is set                               |
| `!flagName`    | flag is not set                           |
| `coins>=N`     | run coin count ≥ N (reads `GameState`)    |

- `set` applies one flag when a choice is taken.
- Unknown condition tokens evaluate **false** and log a warning (fail safe, never throw).

## 3 · Runtime (project scripts)

**`DialogueGraph` (pure C#)** — parsed model: `Id`, `Start`, `Dictionary<string,Node>`;
`Node { Speaker, Text, Goto, Choice[] }`; `Choice { Text, Goto, If, Set }`. A static
`DialogueGraph.Parse(string json)` using `System.Text.Json`. No engine dependency — unit-testable.

**`DialogueState` (pure C#)** — `HashSet<string>` flags + `Dictionary<string,int>`; `SetFlag`,
`HasFlag`, `Evaluate(string condition)`. Reads `GameState.TotalCoins` for `coins>=N`. Static/global
so flags persist across conversations within a run; cleared on new game.

**`Dialogue` (static facade)** —
- `Play(string id)`: `Assets.ReadText` → `DialogueGraph.Parse` → hand to the active `DialogueRunner`.
  No-op with a warning if the file is missing or already in a conversation.
- `IsActive { get; }`, `SetFlag/HasFlag` delegate to `DialogueState`.

**`DialogueRunner` (EntityScript, persistent — `Self.DontDestroyOnLoad()` like `HudController`)** —
- Builds its box UI lazily at runtime (no prefab edit needed), same pattern as the aether bar.
- On `Begin(graph)`: `Time.Pause()`, show box, go to `graph.Start`.
- Per-frame on `Time.UnscaledTime` (so it animates while paused):
  - **Typewriter**: reveal `text` progressively; a click/advance-key while revealing snaps to full.
  - **Advance** (full text shown, linear node): key/click → follow `goto`, or end.
  - **Choices**: spawn one `UIButton` per *visible* choice (condition passed), drive with the
    existing `Ui.SetFocus`/`IsFocused`/`WasActivated` nav; focused choice glows cyan with the `▸`
    marker. Activating a choice applies `set`, follows `goto`.
- On end: hide box, `Time.Resume()`, clear active graph.
- **Pause coordination**: while `Dialogue.IsActive`, `Escape` advances/closes the dialogue and
  `PauseController` must not open the pause menu (add an `IsActive` guard there).
- **Player lock**: `Time.Pause` already freezes physics/enemies; `PlayerController` movement is
  physics-driven so it stops. Add a `Dialogue.IsActive` early-out to its input so no buffered
  jump/draw fires on the frame dialogue closes.

## 4 · Triggers — one component, three entry points

**`DialogueTrigger` (EntityScript)** on an entity that has a 2D trigger collider:
- Fields: `string DialogueId`, `bool Once = true`, `bool RequireInteract = false`.
- Detect the player via `Physics2D` trigger events / overlap against the player entity.
- `RequireInteract == false` → **zone**: fire `Dialogue.Play(DialogueId)` on player enter.
- `RequireInteract == true` → **interact**: while the player is inside, show a small on-theme
  `▸ read` prompt (world-anchored or bottom-center UI hint); fire on the interact key
  (choose a free key — e.g. `E` or `Up`; confirm against `PlayerController` bindings in the plan).
- `Once` removes/disables the trigger after firing.
- **Script-driven** needs no component — any script calls `Dialogue.Play("id")`
  (e.g. from `OnAttach`/level start for a scripted opening).

## 5 · Presentation — matches the menu theme

- **Box:** bottom third, dark ink panel — corner-radius image in the ink palette (near-black
  `~(0.02,0.03,0.05)`), a thin cyan accent rule along the top edge; optionally the `ui_ink_ui`
  material for a wet edge. Sits above the HUD, below the pause overlay.
- **Speaker name:** small **PixelStorm** label in `GameSettings.Accent`, top-left inside the box.
- **Body text:** **IBM Plex Mono Italic**, off-white `~(0.93,0.95,0.97)`, comfortable line
  spacing; typewriter reveal with a soft per-character tick cadence.
- **Choices:** stacked under the body, each a `UIButton`; the focused entry glows cyan and shows
  the `▸` ink marker — identical to `TitleScreen`'s markers.
- **Enter/exit:** box rises + fades from the bottom with a quick ink wash (reuse the ink look;
  a lightweight local version, not the full-screen `ui_ink` flood). Optional faint drips.
- Everything animates on `Time.UnscaledTime`.

## 6 · Build order (phases → tasks)

Each phase ends at an independently verifiable state.

1. **Engine hook** — `aether_assets_read_text` interop + `Assets.cs`; reconfigure; verify a
   script can read a project text asset (log its contents in-editor).
2. **Model + state (pure)** — `DialogueGraph.Parse`, `DialogueState.Evaluate`, condition grammar;
   unit-tested with sample JSON strings (see Testing).
3. **Runner: linear** — `DialogueRunner` box + typewriter + linear `goto` playback; `Dialogue.Play`
   facade; `Time.Pause`/`UnscaledTime`; pause-menu coordination. Verify a linear conversation.
4. **Choices** — visible-choice filtering (`if`), `UIButton` nav, `set` effects, branch jumps.
   Verify a branch changes the following line.
5. **Triggers** — `DialogueTrigger` (zone + interact + `▸ read` prompt + `Once`). Verify all three
   entry points.
6. **Sample + wire-in** — a real `intro.json`, placed in Level1 as a zone trigger and/or on-load
   script call; full end-to-end playtest.

## Testing / verification

- **Pure logic** (`DialogueGraph.Parse`, `DialogueState.Evaluate`) is plain C# — cover with
  focused tests over sample JSON + condition strings. (If no C# test harness exists for the
  project, drive these through a temporary in-editor self-check script that asserts + logs, then
  remove it — same approach used for prior INKBOUND logic.)
- **Runtime/visual**: Play in the editor and drive dialogue with the keyboard via the aethercore
  MCP (`send_input` / `ui_key` / `play_input_sequence`) + `screenshot`. Dialogue advance and
  choice nav are keyboard-drivable, so this works headlessly (mouse cannot be injected). **Never**
  use desktop/computer control to verify.
- Confirm no regressions: run `EngineTests` (only the generic engine hook touches C++), keep FPS
  in the fresh-editor baseline range.

## Out of scope (YAGNI — revisit only on demand)

- Non-blocking ambient "barks" (design leaves room: a future `Blocking=false` mode).
- Portraits / character art, voice/audio, localization.
- Rich variable math or a full expression language beyond the three condition forms.
- A visual dialogue-graph editor (files are hand-authored JSON).
