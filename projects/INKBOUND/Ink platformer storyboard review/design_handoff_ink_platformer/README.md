# Handoff: INKBOUND — Ink Platformer (Storyboard)

## Overview
INKBOUND is a dark, eerie 2D side-scrolling pixel-art platformer built around an **ink-drawing mechanic**: the player draws temporary platforms that must anchor to a solid surface (wall or ground) on both ends, then uses them to cross gaps. This bundle is a **storyboard / art-direction mockup** covering the title screen, level select, in-game HUD + dialogue, a self-talk dialogue system, and a settings screen — establishing tone, layout, and the visual language before the game is built for real.

## About the Design Files
The file in this bundle (`Ink Platformer Storyboard.dc.html`) is a **design reference created in HTML** — a prototype showing intended look, layout, and behavior. It is **not production code to ship**. It's authored as a "Design Component" (a streaming HTML format with an inline template + a `class Component` logic block); treat it as a visual spec, not a source module.

The task is to **recreate these designs in the game's real environment**. For a 2D pixel platformer that most likely means a game engine / framework such as **Godot, Unity, Phaser, LÖVE, or a custom canvas/WebGL renderer** — pick whatever the project already uses, or the most appropriate engine if none exists yet. The HTML/CSS here is a fidelity target for colors, layout, type, and motion, not a DOM to port verbatim. UI screens (menu, settings, dialogue boxes) can be rebuilt in the engine's UI system; gameplay elements describe the intended on-screen composition.

## Fidelity
**High-fidelity for visual direction and UI screens** — colors, typography, spacing, and motion are intentional and should be matched closely. **Directional (lofi) for gameplay geometry** — the cave gameplay frame shows the *intended readable composition* of the ink mechanic (ground → gap → wall → drawn ink bridge → ink well), not exact level coordinates. Use it to nail the visual feel and HUD; real level layout and physics tuning happen in-engine.

## Screens / Views

### 1. Title Screen ("Main Menu")
- **Purpose**: Entry point; sets tone immediately.
- **Layout**: 640×460 framed "screen". Off-center, LEFT-aligned composition (deliberately not centered). Title block pinned top-left (~52px inset). Menu list below it (~188px from top, 66px left). A tiny player sprite stands in the bottom-right corner inside a pool of ink-light.
- **Components**:
  - **Wordmark** "INKBOUND" — Press Start 2P, 38px, letter-spacing 2px, color `#eef1f7`, subtle flicker animation (7s loop). An ink blob (glowing circle + teardrop tail, ink accent color) sits immediately to its left, bobbing (3.5s ease-in-out). Thin vertical ink "drips" fall from the bottom of the wordmark (2px wide, ink color, ~45–55% opacity, one with a bead at the tip).
  - **Tagline** — "the ink knows the way out. you don't." IBM Plex Mono italic, 12px, `#7a8296`.
  - **Menu items** (vertical, gap 20px), each = `[marker] [label] [gloss]`:
    - `descend` — "a new dark" (SELECTED)
    - `return` — "to where you left it"
    - `attune` — "settings"
    - `release` — "quit"
    - Labels: Press Start 2P, letter-spacing 2px. Selected = 17px, ink color, `text-shadow: 0 0 14px <ink>`. Unselected = 15px, `#565c6e`.
    - Marker: selected item has an irregular glowing **ink blot** (organic border-radius `62% 38% 55% 45% / 52% 60% 40% 48%`, ink color, glow) plus two satellite dots; unselected items show a hollow 8px ring (`2px solid #3a4050`).
    - Gloss text: IBM Plex Mono italic, 10px, `#4a5066`, prefixed with "— ".
  - **Wanderer sprite** bottom-right: the pixel character (see Assets) standing over a radial ink-light pool (`radial-gradient(circle, <ink> 0%, transparent 70%)`, ~50% opacity).
  - **Flavor text**: bottom-right "it's looking up at you." (`#3a4050`, italic, 9px); bottom-left "build 0.0.1 — this wasn't supposed to ship" (`#2c2f3a`, 9px).
  - **Ambience**: faint scanlines, a strong vignette (radial darken toward edges), and 2 pairs of faint **red** blinking "watching eyes" (`#6a2a2a`, blink animation) in the dark.

### 2. Level Select
- **Purpose**: Choose a level along a path.
- **Layout**: 640×380 frame, split: left flex area (the node path) + a 200px right detail panel with a 1px left border (`#262b3a`).
- **Components**:
  - **Node path**: a horizontal dashed connector line (`repeating-linear-gradient(90deg,#262b3a 0 8px,transparent 8px 16px)`, 2px) behind a row of 6 circular nodes (44×44, gap 14px). IMPORTANT: the flex container holding nodes needs `min-width:0` so it doesn't overflow the panel.
    - Done nodes (1,2): bg `#1b2030`, border `2px solid #3a4256`, text `#eef1f7`.
    - Current node (3): bg `#1b2030`, border `2px solid <ink>`, glow `0 0 14px <ink>`.
    - Locked nodes (4,5,6): bg `#181a22`, border `2px solid #22242c`, text `#4a5066`, showing a small padlock (a 10×8 arch `border-radius:6px 6px 0 0` over a 16×12 body). Each node has a `title` tooltip: "The Cheerful Plunge", "Quiet, Please", "The Hollow Descent", "it's not ready for you yet", "best not to think about this one", "—".
    - Node label font: Press Start 2P, 12px.
  - **Detail panel**: "SELECTED LEVEL" (Press Start 2P 9px, `#8791a8`), level name "1-3 · The Hollow Descent" (13px 600), a 160×70 striped preview placeholder (`repeating-linear-gradient(135deg,#1b1e28 0 10px,#181a22 10px 20px)`, label "level preview art"), stats ("INK CAPACITY ●●●●○", "PAR TIME 01:40"), and an italic flavor line "the walls here still remember every route you've drawn." (`#4a5066`, 10px).

### 3. Gameplay (Cave) — HUD + Dialogue
- **Purpose**: Show the ink mechanic and the in-game dialogue box.
- **Layout**: 460×344 frame. Top **280px = the stage** (`overflow:hidden`), bottom **64px = the dialogue box**.
- **Stage composition** (from the drawn mockup):
  - Background: near-black gradient, scanlines, heavy radial vignette (`radial-gradient(ellipse at 42% 55%, transparent 26%, rgba(0,0,0,0.78))`), stalactite triangles hanging from the top (CSS border triangles, color `#1a1c24`).
  - **Lurking presence**: a dark radial mass rising from the floor on the right + a pair of pulsing **red** eyes (`#7a1f1f`, glow, `ib-glow-red` 4s pulse). More faint red blinking eyes scattered left.
  - **Ground platform** bottom-left (`170×46`, `#23262f`, top border `#3a3f4d`), a **gap**, then a taller **wall** on the right (`70×120`, `#2c2f3a`).
  - **Ink bridge**: an SVG cubic-bezier path from the ground edge up to the wall top (`d="M150,222 C190,170 230,150 290,148"`), drawn twice — a wide low-opacity glow stroke (12px, 25%) under a dashed 4px stroke (`stroke-dasharray:2 6`, 95%), both in ink color. Small filled circle **anchor dots** at each end.
  - **Player sprite** standing on the ground near the gap.
  - **Ink well pickup**: a glowing ink orb on a small pedestal, with a tiny blinking pupil.
  - **HUD (top-left)**: "INK" label (Press Start 2P 8px) above a 5-segment meter — 4 segments filled (ink color, `0 0 4px` glow), 1 empty (`1px solid #3a4256`). Each segment 14×8.
  - **Dissolve note (bottom-right of stage)**: "dissolves in ~3s (it gets bored)".
- **Dialogue box** (bottom 64px): `background: rgba(6,7,11,0.96)`, top border `2px solid #2c2f3a`, padding 9px 14px. Speaker row = a small ink square + "ME" (Press Start 2P 7px, ink color). Line: `"I don't remember drawing that one."` (11px, `#c8cede`, italic) followed by a blinking block caret `▍` (`ib-caret` 1s steps blink).

### 4. Self-Talk Dialogue (system spec)
- **Purpose**: Documents the character's internal monologue system.
- **Layout**: A panel (max 640px) listing dialogue lines as mini textboxes (bg `rgba(6,7,11,0.9)`, left border `2px solid #2c2f3a`, italic `#c8cede`).
- **Lines**: "It's colder the deeper I draw." / "...I didn't make that platform." / "If I stop drawing, does the floor stop being there?" / "Something down here draws back."
- **Behavior note**: delivered as pixel textboxes, one line surfacing at a time as the player moves; no portrait, just the voice.

### 5. Settings ("attune")
- **Purpose**: Diegetic, ink-stained "slate" — deliberately not a stock form.
- **Layout**: 640×400 frame, padding 30px 44px, left-aligned.
- **Components**:
  - Header "attune" (Press Start 2P 22px, ink color, `text-shadow 0 0 16px <ink>`) with a smudged underline (`linear-gradient(90deg,<ink>,transparent)`, 3px, 60% opacity) and a small ink dot at its end. "< back to the dark" link above (`#565c6e`, 10px). Subtitle: "the dark listens differently to each of these." (italic, `#7a8296`).
  - **Sliders** (Music Volume 70%, SFX Volume 55%, Ink Glow Intensity 85%): label = italic `#c8cede` 13px; track = 210×8 inset groove (`bg #050608`, border `1px #1a1c24`, `box-shadow: inset 0 1px 3px rgba(0,0,0,0.8)`, radius 5px). Fill = ink color with glow, and a **pooling orb** (12px circle, glow) at the fill's leading edge.
  - **Screen shake toggle**: "FULL" label (ink, 9px) + an **ink well** — a 26px inset socket holding a glowing ink orb (13px) when on.
  - **Locked/redacted row**: label "the thing you keep hearing" blurred (`filter:blur(1px)`) under redaction bars (`repeating-linear-gradient(90deg,#1a1c24 0 14px,transparent 14px 20px)`); empty socket at right; whole row 55% opacity.
  - Footer: "some settings settle back on their own after you leave." (`#3a4050`, italic, 9px).

## Interactions & Behavior
- **Menu**: up/down changes selection; the ink-blot cursor moves to and glows on the selected item, others show a hollow ring. `descend` → new game, `return` → level select, `attune` → settings, `release` → quit.
- **Level select**: left/right moves along the path; locked nodes are non-selectable and show their tooltip/flavor; the detail panel updates to the highlighted level.
- **Dialogue**: text types in character-by-character with a blinking block caret; lines advance on input or automatically as the player moves. Tone = wry, unsettled, first-person.
- **Animations** (all subtle, looping):
  - `ib-bob` — logo/idle float, translateY 0→-3px, 3.5s ease-in-out.
  - `ib-flicker` — title opacity flicker, 7s.
  - `ib-blink` — "watching eyes", mostly closed with brief openings, 9–13s (varied delays).
  - `ib-glow-red` — red eyes pulse 0.6↔1 opacity, 4s.
  - `ib-caret` — dialogue caret hard blink, 1s steps.
- **Mechanic rules** (the core loop):
  1. Ink only forms a platform when **both ends anchor** to solid ground/wall; drawing into open air does nothing.
  2. A **5-segment ink meter** caps each draw and drains while drawing; **ink wells** refill it.
  3. Drawn ink **dissolves back to nothing ~3s** after placement — routes are temporary.

## State Management
- `selectedMenuIndex` (title), `selectedLevelIndex` + per-level locked/done/current status (level select).
- Gameplay runtime: `inkMeter` (0–5 segments or a float 0–1), list of active ink strokes each with a `dissolveTimer`, player transform, anchor-validity check per stroke.
- Dialogue: `currentLine`, `typewriterProgress`, queue of pending lines, trigger conditions (position/event based).
- Settings: `musicVolume`, `sfxVolume`, `inkGlowIntensity` (0–1), `screenShake` (bool), plus locked entries.

## Design Tokens
**Colors**
- Background base: `#0d0f14` (page), screen gradients from `#16181f`/`#17191f` → `#08090c`
- Panels / surfaces: `#12141c`, `#1b2030`, dialogue bg `rgba(6,7,11,0.96)`, groove `#050608`
- Borders: `#262b3a` (subtle), `#3a4256`, `#1a1c24`, bezel `#05060a`
- Text: `#eef1f7` (primary), `#c8cede` (dialogue), `#8791a8` / `#7a8296` (muted), `#565c6e` / `#4a5066` / `#3a4050` / `#2c2f3a` (progressively dimmer)
- **Ink accent (tweakable)**: default cyan `#4DD9FF`; alternates `#B084FF` (violet), `#FF6EC7` (pink). Used for ink strokes, meter fill, glows, logo, selection blot, slider fills, toggle orb, headers.
- Horror reds: `#7a1f1f` (glowing eyes), `#6a2a2a` (dim eyes)
- Cave materials: ground `#23262f`, wall `#2c2f3a`, stalactites `#1a1c24`

**Typography**
- Display / UI headers / labels: **Press Start 2P** (Google Fonts). Sizes: 7–8px (micro labels), 10–12px (screen labels), 14–22px (headers), 38px (wordmark). Letter-spacing 1–3px.
- Body / dialogue / gloss: **IBM Plex Mono** (400/500/600), often italic. 9–13px.

**Radius / shadow**
- Screen bezels: 6px radius, `6px solid #05060a` border, `box-shadow: 0 0 0 2px #262b3a, 0 20px 40px rgba(0,0,0,0.5)`.
- Glows: `0 0 Npx <ink>` (N = 4–48 by prominence).
- Inset grooves/sockets: `inset 0 1px–2px 3–4px rgba(0,0,0,0.8)`.

## Assets
- **No external image/icon assets** — everything is CSS/SVG primitives. Replace with real pixel art in-engine.
- **Player sprite**: an 8×10 pixel silhouette rendered via CSS box-shadow "pixels" at 4px each (blocky hooded/robed wanderer). Pattern (X = filled), top→bottom:
  ```
  ..XX....
  .XXXX...
  .XXXX...
  .XXXX...
  ..XX....
  .XXXXX..
  XXXXXXX.
  XXXXXXX.
  XX.X.XX.
  XX...XX.
  ```
  In-engine, replace with a proper 32×40+ (SNES-scale) sprite; the silhouette is directional only.
- **Fonts**: Press Start 2P + IBM Plex Mono via Google Fonts (swap for equivalents already in your project if preferred).

## Files
- `Ink Platformer Storyboard.dc.html` — the full storyboard (all 5 screens). Open in a browser to view. The `class Component` block at the bottom holds the data (menu items, level nodes, slider values, dialogue lines, sprite pattern) and computed style objects; the markup above it is the template.
