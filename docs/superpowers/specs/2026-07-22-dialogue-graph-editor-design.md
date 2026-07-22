# Dialogue Node-Graph Editor — Design (Spec 2)

**Date:** 2026-07-22
**Status:** Approved (brainstorming complete; ready for implementation plan)
**Depends on:** Spec 1 (editor ImGui bridge — `IEditorWindow` + `EditorGui`), shipped.

## Goal

A visual node-graph editor for INKBOUND dialogue, living entirely as an INKBOUND `IEditorWindow`
(project code — no engine/editor changes beyond one generic `Assets.WriteText`). Load a conversation's
JSON, arrange nodes on a canvas, edit every field, add/delete nodes & choices, drag-link `goto`/choices,
and save back to the JSON. Node positions persist in a **sidecar `<id>.layout.json`** so the runtime
data file stays clean.

## Guiding constraints

- **Project boundary:** all editor logic in `projects/INKBOUND/scripts/` using `EditorGui.*`. The only
  generic/SDK addition is `Assets.WriteText(vpath, text)` (symmetric with `Assets.ReadText`,
  editor-writable project dir). See [[aethercore_editor_imgui_bridge]], [[aethercore_project_code_boundary]].
- **Round-trip fidelity:** saving preserves the runtime schema exactly (nodes with `speaker`,
  `portrait?`, `text` with inline `[tag]` markup, `effect?`, and either `goto` or `choices[]` with
  `text`/`goto`/`if?`/`set?`). The editor edits RAW text (tags intact), not the tokenized runs.
- **Non-destructive:** the sidecar holds only positions; a missing sidecar auto-lays-out.
- No shortcuts — a real editor, matching the dialogue runtime's data model ([[aethercore_dialogue_system]]).

## Architecture

One window class `DialogueGraphEditor : IEditorWindow` (+ small helper types), all INKBOUND scripts.

- **Generic SDK add:** `Assets.WriteText(string vpath, string text) : bool` → `aether_assets_write_text`
  → `FileSystem::WriteFileText`. Writes the loose project file in-editor; used for both the dialogue
  JSON and the sidecar.
- **Mutable model (editor-only, distinct from the runtime `DialogueGraph`):**
  - `EdGraph { string Id; string Start; List<EdNode> Nodes; }`
  - `EdNode { string Id; string Speaker; string? Portrait; string Text; string Effect; string? Goto; List<EdChoice> Choices; Vector2 Pos; }`
  - `EdChoice { string Text; string Goto; string? If; string? Set; }`
  - `EdGraph.Parse(json)` (System.Text.Json) and `EdGraph.ToJson()` (indented, runtime schema, omit
    empty optionals). `Pos` comes from the sidecar, not the JSON.
- **Files:** enumerate `assets/dialogue/*.json` (skip `*.layout.json`); a `Combo` picks the active file.
  Load JSON via `Assets.ReadText`, sidecar via `Assets.ReadText` (positions dict); Save writes both via
  `Assets.WriteText`.

## Canvas & interaction (EditorGui draw-list + interaction)

- **Canvas:** a `BeginChild` region; a pan offset (`Vector2`) dragged on empty canvas; optional wheel
  zoom later. World→screen = `nodePos + pan + childOrigin`.
- **Node:** `AddRectFilled` (ink-dark, rounded) + accent title bar + `AddText` (id/speaker + a one-line
  text preview + a choice count). An `InvisibleButton` over the node rect drives selection (click) and
  drag (`IsItemActive` + `MouseDragDelta` → move `Pos`). The Start node gets an accent border.
- **Links:** for a linear `goto`, one `AddBezierCubic` from the node's bottom to the target's top; for
  each choice, a bezier from a per-choice output stub to its `goto` target, labelled with the choice
  text. Missing/`end` targets draw a stub to a faded "end" marker.
- **Drag-link:** press a node's output port (a small circle hit via `InvisibleButton`), drag a live
  bezier to a target node, release to set `goto` (or the choice's `goto`). Releasing on empty canvas
  creates a new node wired to it.
- **Selection side-panel:** the selected node's fields in a right-hand column — `InputText` speaker,
  multi-line-ish `InputText` text (raw, tags visible), `Combo` effect (normal/shake/wave/flicker/
  whisper/glitch), `InputText` portrait + goto; a per-choice editable list (text/goto/if/set + delete),
  an "add choice" button; "delete node" and "set as start" buttons. A toolbar: file combo, "add node",
  "save".
- **Live highlight (play mode):** the runner exposes `Dialogue.CurrentNodeId` (a static it updates on
  `GoTo`); the editor tints that node while playing so you can watch a conversation walk the graph.

## Save format

- **Dialogue JSON** — exact runtime schema (so `DialogueGraph.Parse`/the runner keep working). Node
  order by `Start` first then insertion; optional fields omitted when empty.
- **Sidecar `<id>.layout.json`** — `{ "positions": { "<nodeId>": [x, y], ... }, "pan": [x, y] }`.

## Phasing (Spec 2)

1. **`Assets.WriteText`** generic hook (interop + SDK) — verify a script can write a project text file.
2. **Model + round-trip** — `EdGraph.Parse`/`ToJson`, load a file, save it unchanged, diff to confirm
   fidelity (temp self-check).
3. **Read-only canvas** — window + file combo + node rects + links + pan + node drag + auto-layout +
   save/load sidecar. (Big, useful on its own.)
4. **Node editing** — selection side-panel edits fields + choices; Save writes JSON.
5. **Structural** — add/delete node, add/delete choice, set-start, drag-linking, drag-to-empty-creates.
6. **Live highlight** — `Dialogue.CurrentNodeId` + tint the current node during play.
7. **End-to-end** — edit `intro.json` in the graph, save, play Level1, confirm the runner reflects the
   edits and highlights the walked nodes.

## Verification

In-editor via the aethercore MCP: the window is drawn by the editor's ImGui, so verify with
`screenshot` (canvas, nodes, links, side-panel) and by reading the saved files on disk (round-trip
fidelity). Play to confirm runtime still parses the saved JSON and the live highlight tracks. EngineTests
stay green (only `Assets.WriteText` touches C++). GameRuntime still builds.

## Out of scope

- Multi-file / cross-conversation linking, undo/redo (add if it bites), zoom-to-fit, minimap.
- Editing dialogue for other projects — this window is INKBOUND's; the bridge is what's generic.
