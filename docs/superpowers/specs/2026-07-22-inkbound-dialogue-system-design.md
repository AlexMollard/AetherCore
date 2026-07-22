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
      "portrait": "textures/portraits/void.png",   // optional; reserved slot (art may be placeholder)
      "text": "You should not have come this far [shake]down[/shake].",
      "effect": "whisper",                           // optional whole-line effect
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
- Optional node fields: `portrait` (see §5.1), `effect` (whole-line rich-text effect, see §5.2).
- A **choice** has `text` + `goto`, plus optional `if` (condition) and `set` (effect flag).
- Choices whose `if` evaluates false are **hidden** (not shown greyed).
- **Text is ASCII-only** (font atlas limitation, per the INKBOUND UI gotcha).
- **Inline effect markup** `[effect]...[/effect]` is stripped out during parse into styled runs
  (§5.2); the raw glyph text never shows the tags.

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
`Node { Speaker, Portrait, Text, Effect, Runs[], Goto, Choice[] }`; `Choice { Text, Goto, If, Set }`.
A static `DialogueGraph.Parse(string json)` using `System.Text.Json`. During parse, each node's
`Text` is tokenized into **`Runs` = `TextRun { Text, Effect }[]`** by stripping `[effect]...[/effect]`
markup (§5.2); a node-level `Effect` applies to any run with no inline tag. `Text` retains the plain
tag-free string (for width/typewriter math). No engine dependency — the parser + run tokenizer are
unit-testable in isolation.

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
- Renders each node as speaker + optional portrait (§5.1) + body run element(s) (§5.2), applying
  the `ui_dialogue_text` material to effected runs.
- Per-frame on `Time.UnscaledTime` (so it animates while paused):
  - **Typewriter**: reveal `text` progressively (across runs in order); a click/advance-key while
    revealing snaps to full.
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
  spacing.
- **Choices:** stacked under the body, each a `UIButton`; the focused entry glows cyan and shows
  the `▸` ink marker — identical to `TitleScreen`'s markers.
- Everything animates on `Time.UnscaledTime` (see §5.3).

### 5.1 · Portrait slot (reserved; art may be placeholder)

- The box reserves a **square portrait region** on its left (~box-height), a `Ui.CreateImage`
  with corner radius + a thin accent frame.
- When a node has `portrait`, `Ui.SetImageTexture` loads it and the region shows; otherwise the
  region is hidden and the text column widens to fill.
- This is a **real, wired slot** — data field + layout + load path all exist now. Only the artwork
  is deferred: a single placeholder texture (a dim ink silhouette / "void" face) ships so the slot
  is visibly exercised, and per-speaker art drops in later with no code change.
- Portrait fades/slides in on node change (§5.3).

### 5.2 · Rich text — animated per-glyph effects

Reuses the **material** technique proven by `ui_glitch_text` (a fragment material bound to the
shared `ui_shapes` vertex, masked to real glyph SDF geometry — `type == kShapeSdfGlyph`, `texUV`,
`textureSlot`, screen `position`). A new **project** shader `assets/shaders/ui_dialogue_text.slang`
(INKBOUND-owned, per the boundary rule) selects an effect via a push param:

- `params.x = time`, `params.y = effectId`, `params.z = strength`; `color0/1` = base / accent.
- Effect vocabulary (start small, all driven off `time` + glyph `texUV`/`position` so motion is
  per-glyph, not a rigid block):
  | id | name      | look                                                        |
  |----|-----------|-------------------------------------------------------------|
  | 0  | `normal`  | plain (no material) — the common case                       |
  | 1  | `shake`   | small high-freq jitter of the SDF sample per glyph          |
  | 2  | `wave`    | sine vertical bob phased by glyph column                    |
  | 3  | `flicker` | brightness/alpha noise, occasional dropouts                 |
  | 4  | `whisper` | dim + slow breath + faint chromatic bleed (unsettling calm) |
  | 5  | `glitch`  | the existing broken-signal look (reuse `ui_glitch_text`)    |

  Offsets stay within the glyph's SDF padding (same guard `ui_glitch_text` uses) so sampling never
  bleeds neighbouring atlas glyphs.

**Application:**
- **Whole-line** (node `effect`): the single body text element wears the material with that
  `effectId`. Cheap, high impact — this is the MVP for "make this line spooky."
- **Inline spans** (`[shake]word[/shake]`): the body is split into **run elements** laid out inline.
  IBM Plex **Mono** ⇒ a fixed cell width, so the runner advances x by `cellW * charCount` and wraps
  by column count entirely in C# — no engine measure API needed. Each run is its own text element;
  tagged runs wear the material at their `effectId`, untagged runs render plain. The typewriter
  reveals a global char count across runs in order.
  - Cell width derives from font size × the mono advance ratio (read once from the font meta, or a
    calibrated constant); confirm the exact ratio during the plan.

### 5.3 · Animation requirements (never a single-frame slab)

Every appearance is animated; nothing pops in fully-formed. All on `Time.UnscaledTime`.

- **Box open/close:** rises + fades from the bottom with a quick ink wash (a lightweight local
  version of the ink look, not the full-screen `ui_ink` flood). Reverses on close. Optional drips.
- **Typewriter:** body reveals character-by-character at a configurable rate; a soft per-char tick
  cadence. A click/advance-key while revealing **snaps to full** (then the next input advances).
- **Line-to-line:** advancing to the next node is **not** an instant text swap — the old line
  briefly fades/wipes out, the new speaker/portrait cross-fade in, then the new line types on.
- **Choices:** appear only after the line finishes typing, **staggered** — each choice fades/slides
  up in sequence, not all at once.
- **Speaker + portrait:** fade/slide in on node change.
- **Effect materials** (§5.2) animate continuously while their run is on screen.

## 6 · Build order (phases → tasks)

Each phase ends at an independently verifiable state.

1. **Engine hook** — `aether_assets_read_text` interop + `Assets.cs`; reconfigure; verify a
   script can read a project text asset (log its contents in-editor).
2. **Model + state (pure)** — `DialogueGraph.Parse` (incl. inline-tag run tokenizer),
   `DialogueState.Evaluate`, condition grammar; unit-tested with sample JSON strings (see Testing).
3. **Runner: linear** — `DialogueRunner` box + typewriter + linear `goto` playback; `Dialogue.Play`
   facade; `Time.Pause`/`UnscaledTime`; pause-menu coordination. Verify a linear conversation.
4. **Choices** — visible-choice filtering (`if`), `UIButton` nav, `set` effects, branch jumps.
   Verify a branch changes the following line.
5. **Portrait slot** — reserved region + `portrait` load path + placeholder texture + hidden-when-absent
   layout (§5.1).
6. **Rich text** — `ui_dialogue_text.slang` material with the effect vocab (§5.2); whole-line node
   `effect` first, then inline `[tag]` run splitting (mono cell layout). Verify a line visibly shakes.
7. **Animation polish** — box open/close ink wash, line-to-line cross-fade, staggered choice reveal,
   speaker/portrait fade-in (§5.3). Confirm nothing appears in a single frame.
8. **Triggers** — `DialogueTrigger` (zone + interact + `▸ read` prompt + `Once`). Verify all three
   entry points.
9. **Sample + wire-in** — a real `intro.json` (using a portrait, a whole-line effect, an inline span,
   and a branch), placed in Level1 as a zone trigger and/or on-load script call; full end-to-end playtest.

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
- **Per-speaker portrait artwork** — the slot, data field, and load path are in scope now (§5.1);
  only the finished character art is deferred (a placeholder texture ships).
- Voice/audio, localization.
- Rich variable math or a full expression language beyond the three condition forms.
- Effect vocab beyond the six in §5.2 (add modes to the material as lines demand them).
- A visual dialogue-graph editor (files are hand-authored JSON).
