# INKBOUND — UI Suite Design (Title, Level Select, Settings)

**Date:** 2026-07-21
**Status:** Approved design — ready for implementation planning
**Source of truth for visuals:** `projects/INKBOUND/Ink platformer storyboard review/design_handoff_ink_platformer/` (README.md + `Ink Platformer Storyboard.dc.html`)

## 1. Purpose & Scope

Bring INKBOUND's front-end up to the storyboard: build the three menu screens — **Title**, **Level Select**, and **Settings ("attune")** — as one cohesive, atmospheric menu system in the engine's own UI system.

This is the first sub-project of the larger INKBOUND effort. Explicitly **out of scope** here (each is its own later spec):

- Ink-mechanic refinement (both-ends-anchored strokes, 5-segment meter, ink wells, ~3s dissolve) — the mechanic already exists (`AetherInk.cs` / `InkBlock.cs`) and is untouched by this pass.
- In-game dialogue + self-talk monologue system.
- The gameplay/level art-and-atmosphere overhaul.
- An audio system.

### Decisions locked during brainstorming

| Decision | Choice |
|---|---|
| Fidelity bar | **Full fidelity** — match layout, tokens, *and* the living atmosphere (flicker, bob, drips, blinking eyes, scanlines, vignette, pooling slider orbs). |
| Settings depth | **Wire what's wireable** — Ink Glow and Screen Shake do real things; Music/SFX persist but stay inert (no audio system yet). |
| Fonts | Keep **PixelStorm** for headings/labels; **bake IBM Plex Mono** (Regular + Italic) for the italic "voice" (taglines, gloss, dialogue). |
| Architecture | **One `Menu` scene + state machine** (Title/LevelSelect/Settings roots toggled by a controller); instant transitions; shared shell. |
| Build method | **Author UI structure via the 2D-authoring MCP** into `Menu.scene.toml` (persisted, editor-visible); **scripts drive behavior/animation only**. *Prerequisite:* the engine's MCP can't author UI today (UI Image unreflected; UI components `NOT_ADDABLE`) — Phase 0 of the plan reflects UI Image and makes the UI components addable. |
| Composition | **Full-window**, preserving the deliberate off-center / left-aligned layout; drop the mockup's literal device bezel (a presentation artifact). |
| Level unlock | **All unlocked (dev-friendly)** — nodes 1–4 → Level1–4 all playable; nodes 5–6 locked placeholders; real progression deferred. |
| Ink accent | Cyan `#4DD9FF` constant (storyboard default); Ink Glow scales its glow. |
| Input | Keyboard-first (↑/↓/←/→/Enter) **and** mouse (hover/click), both supported by the UI API. |
| Settings persistence | `%LOCALAPPDATA%/AetherCore/INKBOUND/settings.json`, out of the repo. |

## 2. Architecture

One `Menu` scene (replacing the current simple title scene), full-window. The scene holds the 2D main camera, the shared shell entities, and three screen-root entities. A controller entity runs the state machine.

```
Menu.scene
├── Main Camera (2D)
├── MenuShell            (always visible; behind screens)
│   ├── Background (gradient)
│   ├── Vignette overlay   (topmost layer group, above screens)
│   ├── Scanline overlay   (topmost layer group, above screens)
│   └── WatchingEyes ×N    (red dot pairs, script-blinked)
├── TitleRoot            (shown in Title state)
├── LevelSelectRoot      (shown in LevelSelect state)
├── SettingsRoot         (shown in Settings state)
└── MenuController       (state machine + input routing)
```

**Layering** (back → front): Background → per-screen content → WatchingEyes → Vignette → Scanlines. Vignette/scanlines sit above screen content so every screen reads through the same film.

### Scripts (behavior only — no UI construction at runtime)

Scripts locate authored entities by name/tag (the pattern `HudController` already uses for `GameHud`) and drive them.

| Script | Responsibility | Depends on |
|---|---|---|
| `MenuController.cs` | `Screen { Title, LevelSelect, Settings }` state machine; shows/hides the three roots; routes input to the active screen; global back/quit. | the three screen controllers |
| `MenuShell.cs` | Animates shell atmosphere: watching-eye blink cycles (staggered 9–13s). Static gradient/vignette/scanlines are authored, not scripted. | authored shell entities |
| `TitleScreen.cs` | Selection state (0–3); moves/glows the ink-blot cursor to the selected item; animates wordmark flicker (7s), blob bob (3.5s), drip fall; activates items. | authored TitleRoot entities, `Scene.Load` |
| `LevelSelectScreen.cs` | Node selection with lock-skipping; updates the detail panel on highlight; launches a level on Enter. | node data list, authored LevelSelectRoot entities, `Scene.Load` |
| `SettingsScreen.cs` | Control selection; slider adjust (updates fill + pooling-orb position); toggle flip; reads/writes `GameSettings`. | `GameSettings`, authored SettingsRoot entities |
| `GameSettings.cs` | Static store: `MusicVolume`, `SfxVolume`, `InkGlow`, `ScreenShake`; cyan accent constant; `Load()`/`Save()` to JSON. | `System.IO` |

Each screen controller exposes a small, uniform interface so the controller never reaches into internals:

```
void Build();            // cache authored-entity references, set initial visual state
void Show(); void Hide(); // toggle the screen root's visibility
void HandleInput();      // called by MenuController only while this screen is active
```

## 3. Shared Shell (`MenuShell`)

- **Background**: full-window `ui_image`, base `#0d0f14`, with a subtly lighter top band via a second low-alpha image to fake the `#16181f → #08090c` gradient.
- **Vignette**: generated radial PNG (transparent center → `rgba(0,0,0,~0.8)` edges) into `assets/textures/ui/vignette.png`, full-window `ui_image`, topmost group.
- **Scanlines**: generated 2px repeating horizontal-line PNG into `assets/textures/ui/scanlines.png`, tiled full-window, low alpha, topmost group.
- **Watching eyes**: 2 pairs of small red dots (`#6a2a2a`, corner-radius circles) placed in dark corners; `MenuShell` blinks them on staggered 9–13s cycles (`ib-blink`: mostly closed, brief openings).

## 4. Screens

Colors, type, and spacing follow the storyboard **Design Tokens** section (README §Design Tokens). Key tokens: primary text `#eef1f7`, dialogue/body `#c8cede`, muted `#7a8296`/`#565c6e`/`#4a5066`, borders `#262b3a`/`#3a4256`, panels `#12141c`/`#1b2030`, ink accent cyan `#4DD9FF`, horror reds `#7a1f1f`/`#6a2a2a`.

### 4.1 Title

Off-center, left-aligned composition.

- **Wordmark** "INKBOUND": PixelStorm, large, `#eef1f7`, 7s opacity flicker (`ib-flicker`). A cyan ink-blob (glow circle + small teardrop tail) sits to its left, bobbing on 3.5s (`ib-bob`). Thin cyan drip rects fall from the wordmark base (~45–55% opacity, one with a bead tip).
- **Tagline**: "the ink knows the way out. you don't." — IBM Plex Mono italic, `#7a8296`.
- **Menu items** (vertical, 4): each `[marker] [label] [gloss]`.
  - `descend` — "a new dark" → new game (`Scene.Load("Level1")`)
  - `return` — "to where you left it" → Level Select state
  - `attune` — "settings" → Settings state
  - `release` — "quit" → quit the game
  - Selected: cyan, 17px, glow (`text-shadow 0 0 14px` equivalent), ink-blot marker + 2 satellite dots. Unselected: `#565c6e`, 15px, hollow 8px ring (`2px solid #3a4050`). Gloss: IBM Plex Mono italic, `#4a5066`, "— " prefix.
  - ↑/↓ moves selection; the blot cursor animates to the selected row and glows; Enter activates. Mouse hover selects, click activates.
- **Wanderer sprite**: reuse the existing player sprite (`assets/textures/player`), bottom-right, standing over a radial cyan ink-light pool (~50% opacity).
- **Flavor**: bottom-right "it's looking up at you." (`#3a4050`, italic); bottom-left "build 0.0.1 — this wasn't supposed to ship" (`#2c2f3a`).

### 4.2 Level Select

- **Node path**: horizontal dashed connector (repeating dash segments, `#262b3a`) behind 6 circular nodes (44px, gap 14px).
  - Nodes 1–4 → `Level1`–`Level4`, **all unlocked**. Nodes 5–6 **locked** placeholders (padlock, dimmed `#181a22`/`#22242c`).
  - Current/highlighted node: cyan border + glow. Done/unlocked: `#1b2030` + `#3a4256` border. Node labels: PixelStorm, 12px.
  - Node titles (flavor, from storyboard): "The Cheerful Plunge", "Quiet, Please", "The Hollow Descent", "The Fourth Descent", "it's not ready for you yet", "best not to think about this one".
- **Nav**: ←/→ moves along the path, **skipping locked nodes**; Enter on an unlocked node → `Scene.Load` its target. Mouse hover highlights, click launches.
- **Detail panel** (right, 1px left border `#262b3a`): "SELECTED LEVEL" label, level name (e.g. "1-3 · The Hollow Descent"), a striped preview placeholder (`repeating-linear-gradient` faked with a generated stripe texture or a tinted image), stats ("INK CAPACITY ●●●●○", "PAR TIME 01:40"), italic flavor line. All update on highlight.

### 4.3 Settings ("attune")

- **Header**: "attune" — PixelStorm-scale, cyan, glow, with a smudged underline (gradient bar + ink dot). "< back to the dark" link above (`#565c6e`) → returns to Title. Subtitle "the dark listens differently to each of these." (IBM Plex Mono italic, `#7a8296`).
- **Sliders** (Music 70% / SFX 55% / Ink Glow 85%): label (IBM Plex Mono italic `#c8cede`), inset groove track (`#050608`, `1px #1a1c24`, radius 5px), cyan fill with glow, and a **pooling orb** (glowing circle) at the fill's leading edge. ←/→ adjusts the selected slider; the script updates fill width + orb position.
- **Screen-shake toggle**: "FULL" label + an ink-well socket (26px inset) holding a glowing 13px orb when on; Enter/click flips it.
- **Redacted row**: label "the thing you keep hearing" blurred under redaction bars; empty socket; whole row ~55% opacity; **non-interactive** (skipped by navigation).
- **Footer**: "some settings settle back on their own after you leave." (`#3a4050`, italic) — pure flavor; nothing actually resets.
- **Wiring**: Ink Glow (0–1) → scales the cyan ink accent's glow alpha read by `InkBlock`/`AetherInk`; Screen Shake → a boolean flag gameplay reads where camera shake applies; Music/SFX → persisted only.

## 5. State & Data

- `MenuController`: active `Screen`; delegates input to the active screen controller.
- `TitleScreen`: `selectedMenuIndex` 0–3 (wraps).
- `LevelSelectScreen`: `selectedNodeIndex`; node list `{ name, flavor, targetScene, locked, inkCapacity, parTime }` (6 entries; 1–4 unlocked, 5–6 locked). Navigation clamps/skips locked.
- `SettingsScreen`: `selectedControlIndex` (over the interactive controls only); values read from `GameSettings`.
- `GameSettings` (static): `MusicVolume`, `SfxVolume` (0–1), `InkGlow` (0–1), `ScreenShake` (bool), `Accent` (cyan constant). `Load()` on menu/game start; `Save()` on change. File: `%LOCALAPPDATA%/AetherCore/INKBOUND/settings.json`. Missing/corrupt file → defaults (Music .70, SFX .55, InkGlow .85, ScreenShake true).

## 6. Assets

- **Fonts**: bake IBM Plex Mono Regular + Italic into `assets/fonts/` (`AssetPacker bake-font`). Source TTFs fetched from Google Fonts (OFL, embeddable). PixelStorm stays as-is for headings/labels.
- **Generated textures** (Pillow, matching the existing asset workflow) into `assets/textures/ui/`: `vignette.png` (radial), `scanlines.png` (2px repeat), and if needed a `stripe.png` for the level-preview placeholder.
- **Primitives over textures**: ink blobs, satellite dots, hollow rings, drips, eyes, pooling orbs, node circles, and sockets use `ui_image` corner-radius circles / thin rects wherever possible; only fall back to a texture when a primitive can't express it.
- **Wanderer sprite**: reuse existing `assets/textures/player`.

## 7. Testing / Verification

The engine has no C# game-script test harness, so verification follows the established MCP play-test pattern:

- **Visual**: author each screen, `screenshot`, compare composition + tokens (colors, fonts, spacing) against the storyboard; iterate in-editor.
- **Interaction**: drive `send_input` (↑/↓/←/→/Enter) to verify navigation, blot-cursor tracking, slider adjust, toggle flip, lock-skipping, and level launch; screenshot transitions.
- **Logic**: keep index-wrap, lock-skip, slider-clamp, and settings JSON round-trip as small, self-evident static methods.

### Success criteria

1. Title, Level Select, and Settings each match the storyboard's composition and tokens closely.
2. Keyboard **and** mouse navigation work on all three screens; the ink-blot cursor tracks selection.
3. `descend` loads Level1; nodes 1–4 load Level1–4; nodes 5–6 are locked/non-selectable.
4. Settings persist across an editor restart; Ink Glow visibly changes the ink glow in gameplay; Screen Shake toggles its flag.
5. Full-fidelity animations are present: wordmark flicker, blob bob, drip fall, eye blink, slider pooling-orb.
6. Menu-to-menu transitions are instant with no atmosphere flash; `Menu.scene.toml` holds the authored UI and is committed.

## 8. Risks / Open Notes

- **UI primitive limits**: no arbitrary SVG/bezier or CSS `blur`/organic `border-radius`. The redacted-row blur and organic ink-blot shapes are approximated (small textures or layered rounded rects). Acceptable per "directional where primitives can't reach."
- **Gradient backgrounds**: engine `ui_image` is solid-color + corner-radius + texture; gradients are faked via stacked low-alpha images or a generated texture.
- **Font bake**: if the IBM Plex Mono TTF fetch/bake fails, fall back to PixelStorm for the italic voice and revisit — this is the one external-asset dependency.
- **Screen-shake consumer**: the flag is defined here; the actual camera-shake application may need a small hook in `CameraFollow` (in-scope as a one-line read, not a new system).
