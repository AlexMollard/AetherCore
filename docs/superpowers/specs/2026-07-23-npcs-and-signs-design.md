# NPCs & Signs — Design

**Goal:** Add interactable NPCs and signs to INKBOUND that start conversations through the existing dialogue system.

**Approach:** An NPC or sign is a visible entity + a `DialogueTrigger` in interact mode. The dialogue machinery (`DialogueTrigger` → `Dialogue.Play` → `DialogueRunner`) already exists; this feature adds presentation, a bit of NPC life, and drop-in authoring. All code lives in `projects/INKBOUND` — no engine/app/SDK changes.

## Components

- **`DialogueTrigger` tweak** — add an optional `PromptLabel` field (default `"> read"`) so the interact prompt can read `"> talk"` for NPCs. Only change to existing code.
- **`Npc` component** (`scripts/Npc.cs`, new) — faces the player via `SpriteRenderer.SetFlipX`, plus a gentle idle bob (small vertical sine on the sprite). Assumes a sibling `SpriteRenderer` + `DialogueTrigger`. Signs need no new component (static).

## Prefabs (`assets/prefabs/`)

- `Npc.prefab.toml` — `SpriteRenderer`(npc texture) + `DialogueTrigger`(RequireInteract=true, PromptLabel="> talk", Once=false) + `Npc`.
- `Sign.prefab.toml` — `SpriteRenderer`(sign texture) + `DialogueTrigger`(RequireInteract=true, PromptLabel="> read", Once=false).
- Per-instance override: `DialogueId`, sprite, position.

## Placeholder art (MCP pixel tools)

- `assets/textures/npc_wraith.png` — hunched ink-wraith figure.
- `assets/textures/sign_post.png` — ink-scrawled marker board.
- Ink-horror palette: void black, bone white, amber accent (`GameSettings.Accent`).

## Sample dialogues (`assets/dialogue/`)

- `npc_wraith.json` — short eerie branching chat.
- `sign_warning.json` — scrawled warning (linear).

## Level1 demo

Place one NPC + one sign near the Level1 start (hand-appended entities referencing the prefabs; delete the stale `.scene.bin`). Walk up, press **E**, conversation runs.

## Verification

Load Level1 → Play → walk to each → press E (MCP `send_input`) → confirm the conversation runs → screenshot.

## Boundaries / gotchas

- Project code only; reuse `DialogueTrigger` + `Dialogue`.
- Hand-append scene entities (avoid `save_scene` v16 churn); delete stale `.scene.bin`.
- Author bool script props in toml (MCP bool seeding is unreliable).
