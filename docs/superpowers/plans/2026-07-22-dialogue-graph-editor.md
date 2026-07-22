# Dialogue Node-Graph Editor — Implementation Plan (Spec 2)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Checkbox steps.

**Goal:** A visual dialogue node-graph editor as an INKBOUND `IEditorWindow` (project code) on the Spec 1 `EditorGui` surface — load/arrange/edit/save conversations, positions in a sidecar.

**Spec:** `docs/superpowers/specs/2026-07-22-dialogue-graph-editor-design.md`. **Depends on** the shipped editor ImGui bridge ([[aethercore_editor_imgui_bridge]]).

## Global Constraints

- All editor logic in `projects/INKBOUND/scripts/`; the only engine/SDK touch is generic `Assets.WriteText` (Task 1).
- Round-trip must preserve the runtime dialogue schema exactly (the runner keeps parsing it). Edit RAW text (tags intact).
- Verify in-editor via aethercore MCP `screenshot` + reading saved files on disk; never desktop control.
- New interop `.cpp` → cmake reconfigure. SDK-style csprojs auto-glob `.cs`. Finish by merge+push to master. Address the user as "Pog Champ".

## Task 1: Generic `Assets.WriteText`

**Files:** `src/app/scripting/interop/AssetsExports.cpp` (add fn), `managed/AetherCore/Internal/Native.cs`, `managed/AetherCore/Assets.cs`.

- [ ] **Step 1: Interop.** In `AssetsExports.cpp` add:
```cpp
#include "io/FileSystem.hpp" // already included
// Writes UTF-8 text to a VFS path (loose project dir in-editor). Returns 1 on success, 0 on failure.
AE_SCRIPT_API std::int32_t aether_assets_write_text(const char* vpath, const char* text)
{
	if (vpath == nullptr) { return 0; }
	const auto r = aether::io::FileSystem::WriteFileText(vpath, text != nullptr ? text : "");
	return r ? 1 : 0;
}
```
- [ ] **Step 2: P/Invoke.** In `Native.cs` near `aether_assets_read_text`:
```csharp
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int aether_assets_write_text(string vpath, string text);
```
- [ ] **Step 3: SDK API.** In `Assets.cs`:
```csharp
    /// <summary>Write UTF-8 text to a data asset (loose project dir in-editor). Returns false on failure.
    /// Intended for editor tooling; a shipped read-only pak will fail gracefully.</summary>
    public static bool WriteText(string virtualPath, string text) => Native.aether_assets_write_text(virtualPath, text) != 0;
```
- [ ] **Step 4: Reconfigure + build + verify.** `cmake -S . -B build/vs2022-msvc` then build Editor. Confirm no `WriteFileText` signature mismatch (check `io/FileSystem.hpp` returns `Expected<void>`; `if (r)` works). Verify round trip later in Task 2.
- [ ] **Step 5: Commit** `Add generic Assets.WriteText`.

## Task 2: Editor model + JSON round-trip

**Files:** Create `projects/INKBOUND/scripts/EdDialogue.cs` (EdGraph/EdNode/EdChoice + Parse/ToJson). Temp: `DialogueGraphSelfTest.cs`.

- [ ] **Step 1:** Write `EdGraph`/`EdNode`/`EdChoice` (mutable; `Pos` on EdNode is `Vector2`, not serialized to JSON). `EdGraph.Parse(string json)` via `System.Text.Json` reading the runtime schema (id, start, nodes{ speaker, portrait?, text, effect?, goto?, choices[]{text,goto,if?,set?} }). `ToJson()` via `System.Text.Json` `JsonSerializer` or a hand-built `Utf8JsonWriter` with indentation, **omitting** empty `portrait`/`effect`(when "normal")/`goto`/`if`/`set` and writing `choices` only when non-empty, `start` first.
- [ ] **Step 2: Round-trip self-test.** Temp `IEditorWindow`-free `EntityScript` (or reuse the in-editor self-test pattern): parse `intro.json` (via `Assets.ReadText("project://assets/dialogue/intro.json")`), `ToJson()`, re-`Parse()`, assert node/choice/field equality; log `[EDTEST] PASS/FAIL`. Attach, Play, read console. Expect all PASS.
- [ ] **Step 3:** Remove the self-test. **Commit** `Add editor dialogue model + JSON round-trip`.

## Task 3: Read-only canvas (the big one)

**Files:** Create `projects/INKBOUND/scripts/DialogueGraphEditor.cs` (`IEditorWindow`).

- [ ] **Step 1:** Window skeleton: `Begin("Dialogue Graph", ref _open)`; toolbar row — a `Combo` of `assets/dialogue/*.json` filenames (enumerate: there's no directory-list API from C#, so keep a known list or add a tiny `Assets.List` later; for now hardcode-scan known ids by trying `Assets.ReadText` on a small candidate set OR add `Assets.ListDialogue` in a follow-up — simplest: read a manifest or list the files the project ships). On selection, `Load(id)`.
- [ ] **Step 2: Load.** `Assets.ReadText(dialogue json)` → `EdGraph.Parse`; `Assets.ReadText(sidecar .layout.json)` → positions dict (assign to `EdNode.Pos`); missing sidecar → auto-layout (simple layered/grid by BFS from Start).
- [ ] **Step 3: Canvas.** `BeginChild("canvas")`; pan offset dragged on empty space (`InvisibleButton` over the whole canvas background, `IsItemActive` + `MouseDragDelta` → pan). For each node: compute screen rect = `Pos + pan + childOrigin`; `AddRectFilled` + accent title bar + `AddText` (id/speaker + one-line text preview + choice count); Start node gets an accent border; an `InvisibleButton` per node for select (click) + drag (`IsItemActive` → move `Pos`).
- [ ] **Step 4: Links.** For `goto` and each choice.goto, `AddBezierCubic` node-bottom → target-top; choice links labelled with `AddText`; missing/`end` → faded stub.
- [ ] **Step 5: Save layout.** A "Save Layout" button → write sidecar `{positions, pan}` via `Assets.WriteText`. (Full JSON save comes in Task 4.)
- [ ] **Step 6: Verify in-editor.** Launch editor, open the window, pick `intro`, `screenshot` — confirm nodes, links, drag a node, save layout, re-open, positions persisted (read the sidecar on disk). **Commit** `Add read-only dialogue graph canvas`.

## Task 4: Node editing + save JSON

- [ ] **Step 1:** Right-hand selection panel (`BeginChild` or same window column): for the selected `EdNode`, `InputText` speaker/text(raw)/portrait/goto, `Combo` effect; per-choice list — `InputText` text/goto/if/set each + a delete button; "add choice" button.
- [ ] **Step 2:** Toolbar "Save" → `EdGraph.ToJson()` → `Assets.WriteText(dialogue json)`; also save the sidecar.
- [ ] **Step 3: Verify.** Edit a line + a choice, Save, read the JSON on disk (fields updated, schema intact), and Play Level1 to confirm the runner shows the edit. **Commit** `Add node editing and JSON save`.

## Task 5: Structural editing + drag-linking

- [ ] **Step 1:** Toolbar "Add Node" (new id, placed at canvas center); node panel "Delete Node" (repoint danglers to none) + "Set as Start".
- [ ] **Step 2:** Output ports: a small circle per node (linear) / per choice, hit via `InvisibleButton`; press+drag draws a live bezier to the mouse; release over a node sets `goto`; release on empty canvas creates a node wired to it.
- [ ] **Step 3: Verify.** Add a node, drag-link it from a choice, save, Play, confirm the branch reaches the new node. **Commit** `Add structural editing and drag-linking`.

## Task 6: Live current-node highlight

**Files:** `projects/INKBOUND/scripts/Dialogue.cs` (+ `DialogueRunner.cs`).

- [ ] **Step 1:** Add `public static string? CurrentNodeId` to `Dialogue`; `DialogueRunner.GoTo` sets it (and clears on end). 
- [ ] **Step 2:** In the editor, while `Dialogue.IsActive`, tint the node whose id == `Dialogue.CurrentNodeId`.
- [ ] **Step 3: Verify.** Open the graph, Play Level1, walk the intro, watch the highlight move node-to-node. **Commit** `Highlight the runner's current node in the graph`.

## Task 7: Finalize

- [ ] EngineTests green (`Assets.WriteText` only C++), GameRuntime builds.
- [ ] Revert stray scene/asset migrations.
- [ ] Merge + push to master (superpowers:finishing-a-development-branch).

## Self-review notes

- Boundary: only generic touch is `Assets.WriteText` (T1); all else is INKBOUND scripts.
- Round-trip fidelity gated by the T2 self-test before any UI is built on the model.
- Open verification (not a placeholder): the dialogue-file enumeration for the Combo (T3 Step 1) — no C# directory API yet; resolve by shipping a small `Assets.ListDir(vpath)` generic hook OR a project manifest during T3, decided when reached. Flagged.
- Type consistency: `EdGraph/EdNode/EdChoice`, `DialogueGraphEditor`, `Dialogue.CurrentNodeId` used consistently.
