# INKBOUND — Save, Slots & Progression — Design

**Date:** 2026-07-24
**Status:** Approved (design), pending implementation plan
**Scope:** Pure project C# under `projects/INKBOUND/`. No engine or SDK changes.

## Purpose

INKBOUND currently persists nothing about a playthrough. `GameState` is a set of
process-lifetime statics (coins, totals, win flag), Level Select hardcodes every real
level as unlocked, and there is no notion of "where you left off". This feature adds a
persistent, multi-slot save profile so that progression (completion, unlocks, records,
and mid-level resume) survives a quit, and makes Level Select's locking meaningful.

A visible run timer was deliberately **rejected** for the story playthrough (it fights a
story-driven first descent). Timing survives only as an opt-in **time-trial** mode that
unlocks per level after that level is first completed.

## Constraints & Context

- **No engine/SDK work.** Project C# can use the .NET BCL directly. The persistence
  pattern is already proven in `projects/INKBOUND/scripts/GameSettings.cs`
  (`System.IO` + `System.Text.Json`, writing to
  `%LocalAppData%/AetherCore/INKBOUND/`). Reuse it verbatim.
- **Settings stay global**, not per-slot. `settings.json` is unchanged. Save profiles are
  new, separate files.
- **The counted collectible is coins** (`GameState.Coins` / `TotalCoins`). Aether crystals
  (`AetherCrystal.cs`) are a *respawning ink-refill resource*, not a tally, and stay that
  way — "best collectibles per level" means best coins. Making crystals countable is an
  explicit non-goal here.
- **Levels are identified by scene key** (`"Level1"`..`"Level4"`), a stable save key.
- **Completion hook** is `GoalFlag.OnTriggerEnter2D` → `GameState.Win()`. Level chaining
  via `GoalFlag.NextScene` is unchanged.

## Architecture

Three project-C# units, mirroring the existing settings design:

- **`SaveProfile`** — plain serializable data for one slot (below). Pure data + a couple
  of derived helpers; no engine calls.
- **`SaveSystem`** (static) — the single owner of on-disk state. Loads all three slots on
  boot, exposes the **active slot**, and writes a slot on demand. Isolated file I/O so the
  rest of the game never touches disk directly.
- **`GameState`** — extended to hold a reference to the active `SaveProfile` and to route
  completion / checkpoint / coin events into it, autosaving through `SaveSystem`.

### File layout
```
%LocalAppData%/AetherCore/INKBOUND/
  settings.json     (existing, global, unchanged)
  slot0.json        (new)
  slot1.json        (new)
  slot2.json        (new)
```
A slot file absent on disk = empty slot. Load is best-effort: a corrupt/missing file is
treated as empty and logged as a warning (same try/catch discipline as `GameSettings`).

## Data Model

`SaveProfile` (one per slot):

| Field | Type | Notes |
|---|---|---|
| `Exists` | bool | False = empty slot ("a new dark"). |
| `FurthestLevel` | string | Scene key of the deepest level reached, for `return`. |
| `Levels` | map/array keyed by scene key | Per-level record (below). |

Per-level record (`Level1`..`Level4`):

| Field | Type | Notes |
|---|---|---|
| `Completed` | bool | Set true when the level's goal is reached. |
| `BestCoins` | int | `max` of coins collected on any completed run of this level. |
| `BestTimeSeconds` | float? | Null until a **time-trial** completion; story runs never set it. |
| `FurthestCheckpoint` | int | Highest checkpoint ordinal reached this attempt; used for resume. `0` = no checkpoint reached yet (resume at the level's start spawn). Reset to `0` on level completion. |

**Unlock is derived, never stored.** A level is unlocked iff it is `Level1` **or** the
previous level's `Completed` is true (strictly linear). The two "not ready" Level Select
nodes (indices 4–5) remain permanent flavor locks, independent of the profile.

## Flows

### Boot
`SaveSystem` loads all three slots. No active slot yet.

### `descend` (Title)
→ **SlotSelect in new-run mode.** Pick a slot:
- Empty slot → becomes active, starts `Level1` fresh.
- Used slot → confirm-overwrite prompt → wipe the slot to empty → becomes active → start
  `Level1`.

### `return` (Title)
→ **SlotSelect in resume mode.** Only used slots are selectable. Selecting one makes it the
active slot and loads its `FurthestLevel` at its `FurthestCheckpoint`.

### Selecting a slot
The chosen slot becomes `SaveSystem.Active`. `GameState` binds to it for the session; every
subsequent autosave writes that slot.

### Checkpoint reached (in gameplay)
`Checkpoint.OnTriggerEnter2D` records the checkpoint ordinal into the active profile's
`FurthestCheckpoint` (and updates `FurthestLevel` if this level is deeper) → autosave.
Each `Checkpoint` gains an authored **`Index` int** (ascending along the level) so resume
can match the right one; checkpoints currently carry no id.

### Level complete
`GameState.Win()` (via GoalFlag): set the level's `Completed`, `BestCoins = max(BestCoins,
Coins)`, reset that level's `FurthestCheckpoint`, and — because unlock is derived — the next
level is now unlocked automatically → autosave. Existing `NextScene` chaining is untouched.

## New Screen: SlotSelect

A new `SlotSelect` scene + `SlotSelectScreen` script, styled with the same `ui_ink_ui`
brushed-ink material as Level Select so it reads as part of the menu suite. Three slot rows;
each shows either "a new dark" (empty) or a summary: furthest-level name, best-coin total,
and completion count (e.g. `1-3 The Hollow Descent — 6 coins — 2/4 done`). One screen,
entered by both `descend` and `return` with a **mode flag** that controls whether empty
slots are selectable and whether an overwrite confirm is shown.

Navigation reuses the engine `UiNavigationSystem` + `Ui.IsFocused` / `Ui.WasActivated`
pattern already used by `LevelSelectScreen` and `TitleScreen`. The overwrite confirm is a
small in-screen two-choice prompt (no new scene).

## Level Select Changes

Replace the hardcoded `Locked` array in `LevelSelectScreen.cs` with per-active-slot state:

- **Lock** levels that are not unlocked (derived rule above); locked real levels are
  non-selectable, matching today's placeholder-node styling.
- **DONE badge + best coins** shown on completed nodes; the detail panel shows best coins
  (and best time if a trial has been run).
- **Time-trial affordance** on a completed node: pressing **T** flips that node into timed
  mode; `descend`-ing it then runs the level as a time trial.

## Time-Trial (post-completion timer)

- Story runs have **no clock and no timer state** whatsoever.
- A completed level's node can be toggled into time-trial. That run shows a HUD clock and,
  on completion, records `BestTimeSeconds = min(existing, thisRun)` → autosave.
- Trial-mode clock rules: the clock runs from level start to the goal; **deaths cost time**
  (respawn does not reset it); the clock **freezes during dialogue and the pause menu**
  (which already freeze the world). These rules apply in trial mode only.

## Testing

Pure-logic units are unit-tested in the project's C# test surface where one exists;
otherwise verified via a scripted playtest through the aethercore MCP:

- **`SaveProfile` / unlock derivation**: level 1 always unlocked; level N unlocked iff N-1
  completed; completion sets flags and best-coin max correctly; best-time min only updates
  downward and only from trial runs.
- **Round-trip persistence**: write a profile, reload from disk, fields match; a
  missing/corrupt file loads as an empty slot without throwing.
- **Resume**: reaching checkpoint index k in Level2, quitting, and `return` lands the player
  at Level2's checkpoint k.
- **Overwrite**: `descend` onto a used slot, after confirm, clears its records.
- **End-to-end playtest**: fresh slot → beat Level1 → Level2 unlocks in Level Select → quit
  → `return` resumes → time-trial a completed level and see a recorded best time.

## Out of Scope

- **Audio** — the Settings volume sliders remain non-functional; audio is a separate,
  engine-level feature tracked elsewhere.
- **Aether crystals as a collectible tally** — they stay a respawning refill resource.
- **Cloud/remote saves, save export/import, achievements.**
