# Undo System Refactor Plan

Replace the coarse `EntityDiffCommand` (whole-scene snapshot-diff) with fine-grained
typed commands for every scene-mutating operation.

## Architecture

All commands implement `IEditorCommand` (`Undo`, `Redo`, `Label`, `Remap`).
Recorded via `UndoStack::Record()`. Handlers access undo through
`ctx.services.TryGet<UndoStack>()`.

Two reflection paths exist for component ops:
- Reflected: `reflect::FindComponentType(name)` → `FieldDesc::get/set`
- Hand-authored: `editor::FindComponentFields(name)` → `read/write` JSON

Key helpers:
- `editor::FieldValueToJson()` / `editor::JsonToFieldValue()` in `ReflectionJson.hpp`
- `editor::EnableComponentFeatures(world, *entry)` after catalog add
- `rt->postSet(world, entity)` after reflected field writes
- `CaptureComponentFields()` / `ApplyComponentFields()` free functions in `EditorCommand.hpp`

## Phase 1 ✅ — RenameCommand + ReparentCommand

**Files modified:**
- `src/app/debug/EditorCommand.hpp` — class declarations
- `src/app/debug/EditorCommand.cpp` — implementations
- `src/app/editor/ControlMethods.cpp` — wiring + exclusion list entries

**Commands:**
- `RenameCommand(entityId, oldName, newName)` — stores id + names, undo/redo applies via EmplaceOrReplace
- `ReparentCommand(entityId, oldParentId, newParentId)` — stores ids, undo/redo calls ecs::SetParent

**Wiring:**
- `scene.rename` handler records `RenameCommand`
- `scene.parent` handler records `ReparentCommand`
- Both added to wrapping loop exclusion list

## Phase 2 ✅ — Component Commands (Add/Remove/Set)

**Files modified:**
- `src/app/debug/EditorCommand.hpp` — 3 class declarations + `CaptureComponentFields`/`ApplyComponentFields` free function declarations
- `src/app/debug/EditorCommand.cpp` — includes fixed (`nlohmann/json.hpp` not `json_fwd.hpp`), anonymous-namespace helpers (`CaptureComponentFields`, `ApplyComponentFields`), 3 command implementations
- `src/app/editor/ControlMethods.cpp` — `componentOp` lambda wired for Add/Remove, `setComponent` lambda wired for Set, 6 exclusion list entries

**Commands:**
- `AddComponentCommand(entityId, componentName)` — undo removes, redo adds via `catalog.find(name)->add()` + `EnableComponentFeatures`
- `RemoveComponentCommand(entityId, componentName, snapshot, isReflected)` — captures JSON fields before removal, undo re-adds + restores, redo removes
- `SetComponentCommand(entityId, componentName, before, after, isReflected)` — captures before/after JSON of changed fields, undo/redo applies via `ApplyComponentFields`

**Build fixes applied:**
1. `json_fwd.hpp` → `json.hpp` (fwd decl insufficient for by-value `nlohmann::json` members)
2. Unqualified `json` → `nlohmann::json` in constructor params
3. Removed spurious `} // namespace` at line 583 that prematurely closed `namespace aether::editor`

**Build status:** 0 errors, 0 warnings

## Phase 3 ✅ — ScriptCommand (add_script / remove_script)

**Files modified:**
- `src/app/debug/EditorCommand.hpp` — `AddScriptCommand` and `RemoveScriptCommand` declarations + `#include "scene/Components.hpp"`
- `src/app/debug/EditorCommand.cpp` — both implementations after `SetComponentCommand`
- `src/app/editor/ControlMethods2D.cpp` — script handler wiring + `#include "debug/EditorCommand.hpp"`
- `src/app/editor/ControlMethods.cpp` — 2 exclusion list entries added

**Commands:**
- `AddScriptCommand(entityId, scriptType, scriptEntry)` — stores entity id, type name, and full `ScriptEntry`. Undo removes last matching script; redo re-adds the stored entry.
- `RemoveScriptCommand(entityId, scriptType, scriptEntry)` — captures matching `ScriptEntry` before removal. Undo re-adds it; redo erases all matching.

**Wiring:**
- `scene.add_script` handler records `AddScriptCommand` after appending the new entry
- `scene.remove_script` handler snapshots first matching `ScriptEntry`, records `RemoveScriptCommand`
- Both added to wrapping loop exclusion list

**Build status:** 0 errors, 0 warnings

## Phase 4 ✅ — SubtreeLifetimeCommand for Prefab/Model Ops + UnpackPrefabCommand

**Files modified:**
- `src/app/debug/EditorCommand.hpp` / `.cpp` — `UnpackPrefabCommand` declaration + implementation
- `src/app/editor/ControlMethods.cpp` — wiring + exclusion list entries

**Wiring:**
- `scene.add_model` → records `SubtreeLifetimeCommand` (`createdByThisEdit=true`) on the spawned root
- `scene.add_prefab_instance` → records `SubtreeLifetimeCommand` on the instantiated root
- `scene.revert_prefab_instance` → records an old-subtree delete + new-subtree create pair (destroy old, instantiate new, one command each)
- `scene.unpack_prefab_instance` → records `UnpackPrefabCommand`, which snapshots the prefab linkage
  (`PrefabInstanceComponent` on the root + per-node `PrefabLinkComponent` / `SceneTransientComponent`)
  before it is stripped, and re-applies it **in place** on undo — no destroy/recreate, so ids survive
- `scene.apply_prefab_instance` → **excluded, records nothing**: it only writes the prefab file, so
  there is no scene state to undo (reverting a file write is out of scope)

## Phase 5 ✅ — SceneReplaceCommand for load/new

**Files modified:**
- `src/app/debug/EditorCommand.hpp` / `.cpp` — `SceneReplaceCommand`
- `src/app/editor/ControlMethods.cpp` — `scene.load` / `scene.new` wiring + exclusion entries

**Command:**
- `SceneReplaceCommand(before, after, beforeSceneName, afterSceneName)` — stores two full
  `SceneDescription` snapshots + scene names; `Apply()` calls `app::scene::ReplaceScene` and
  updates `SceneSubsystem::SetCurrentScene`. Undo restores `before`, redo re-applies `after`.

## Phase 6 ✅ — EntityDiffCommand and the wrapping loop removed

Completed **after** Phase 7 below, which was its real precondition. Removed:

- `EntityDiffCommand` (class + implementation, and its diff/group helpers)
- `UndoStack::CaptureBaseline` / `CommitPending` / `Push` / `CaptureScene`, plus
  `m_pendingBefore` / `m_pendingBeforeKey`
- The `ControlMethods.cpp` wrapping loop and its exclusion list
- `DebugLayer`'s per-frame baseline-on-click / commit-at-frame-end
- The six `EntityDiffCommand` test cases and the scene-capture scaffolding they needed

`AbandonPending()` survives, repurposed: it now drops an in-flight *coalesced field edit*
when the editor stops being editable (entering play/compile).

**Latent bug fixed on the way out.** `Record()` used to call `AbandonPending()`, which
*discarded* any in-flight field edit — so a command landing mid-drag (deleting an entity
while a slider was held) silently swallowed the drag. `Record()` now flushes instead, and
`FlushFieldEdit()` calls the private `RecordCommand()` so the two cannot recurse.

### The original (wrong) analysis, kept for context

**The plan's premise was wrong.** `EntityDiffCommand` is not merely the MCP fallback — it is the
undo mechanism for **direct, in-editor manipulation**, which was never converted to typed commands:

- `DebugLayer.cpp` — per-frame `CaptureBaseline()` on left-click / `CommitPending()` at frame end.
  This is what makes **gizmo drags, inspector field edits, and every ImGui-driven scene edit** undoable.
- `ViewportPanel.cpp` — `Push()` for 2D collider handle editing.
- `HierarchyPanel.cpp` — `Push()` for hierarchy cut / paste / delete.

Removing `EntityDiffCommand` would silently break undo for all of the above. It also can't be called
"coarse" anymore — it is already a **surgical subtree diff** (only touched subtrees are restored),
so keeping it as the generic net for interactive editing is the correct design, not a stopgap.

**Revised recommendation — do NOT remove EntityDiffCommand yet.** The MCP-side conversion
(Phases 1–5) is complete. The remaining MCP methods still on the net are asset-only writes
(`atlas.slice`, `animation.create`, `tiles.add_layer`) whose scene diff is empty, plus
`tiles.create` (creates assets + binds one component). Removing the net additionally requires
Phase 7 below to finish.

**Known trade-off (not a regression to fix now):** batch MCP ops (`*_many`) now record one typed
command *per item*, so a 10-entity batch is N undo entries — consistent with the pre-existing
`transform_many` / `create_many` / `delete_many` behavior. A `CompositeCommand` grouping a batch
into a single undo entry is the proper fix if atomic batch-undo is wanted.

## Phase 7 — Interactive (in-editor) edit paths

Converting the ImGui-driven edit paths off the `CaptureBaseline`/`CommitPending` catch-all.
Each converted path stops relying on the diff automatically: `UndoStack::Record()` drops the
pending baseline, so `CommitPending` finds nothing to commit. That makes this incremental —
the net stays in place until the last path is converted.

### Done

| Path | Command | Notes |
|---|---|---|
| Viewport gizmo drag | `TransformCommand` | Pre-drag matrices re-snapshotted while idle (ImGuizmo only flags "using" *after* a drag starts); recorded on release, so a multi-frame drag is one step |
| Hierarchy cut | `SubtreeLifetimeCommand` | Captured before destroy |
| Hierarchy paste | `SubtreeLifetimeCommand` | Captured after the paste offset lands |
| Hierarchy duplicate | `SubtreeLifetimeCommand` | |
| Hierarchy delete | `SubtreeLifetimeCommand` | Now captures selection *roots* so nested selections aren't double-captured |
| Hierarchy drag-drop | `HierarchyMoveCommand` (new) | Reparent **and** sibling reorder are both `InsertChildAt`; stores old/new (parent, index) so undo restores the exact slot |
| Collider 2D handles | `SetComponentCommand` | Snapshots the reflected "Collider 2D" fields once per gesture |
| Viewport texture drag-drop | `SubtreeLifetimeCommand` | |
| Inspector reflected fields | `SetComponentCommand` | Central hook in `ReflectedComponentDrawer`; `DrawField` already reads the pre-edit value, so before/after is free |
| Inspector reflected remove (X) | `RemoveComponentCommand` | Snapshots fields so undo restores values, not just presence |

**Drag coalescing:** `UndoStack::RecordFieldEdit()` accumulates the per-frame stream (earliest
`before` per field, latest `after`); `FlushFieldEdit()` — driven from `DebugLayer` when
`!ImGui::IsAnyItemActive()` — emits one command. A drag returning to its start records nothing.

### Done — the bespoke drawers

The 14 hand-written drawers in `ComponentDrawers*.cpp` (~2.6k lines, ~93 mutating widgets) are
covered **without hooking any individual widget**. 13 of the 14 components are still *reflected* —
they are only excluded from the reflected *drawing* pass — so `InspectorPanel` snapshots the
entity's fields around the whole drawer block and diffs afterwards, feeding each change into the
same coalescing buffer. One hook, not 93.

| Surface | Mechanism |
|---|---|
| 13 reflected components (Transform, Camera, Rigid Body, Collider, Joint, Sprite Renderer/Animator, UI Canvas/Rect/Image/Text, Skinned Mesh, Name) | Field diff → `SetComponentCommand` |
| Material (the one non-reflected component) | Same diff via its hand-authored `ComponentFieldSet` |
| Scripts (type, properties, slot add/remove) | `SetScriptsCommand` (new) — whole before/after list |
| Tags | `SetTagsCommand` (new) — whole before/after tag set |
| Add-component palette | `AddComponentCommand` (+ one for the implicit Transform) |
| Hierarchy script drop | `SetScriptsCommand` at that site (outside the inspector's scope) |

Three details that matter:

- **Multi-selection.** `DrawTransform` applies its delta to every selected entity, so the snapshot
  covers the whole selection — recording only the primary would drop the baseline and silently lose
  the rest. The coalescing buffer therefore holds one entry per `(entity, component)` and flushes
  them together, instead of flushing whenever the target changes.
- **`attached` is runtime state.** `ScriptListsEqual` ignores it; comparing it would register a
  phantom edit every time the script system attaches. `SetScriptsCommand` also clears it on
  restore, since the managed instances behind the old list are gone.
- **The snapshot is gated** on a widget being active/clicked, so idle frames cost nothing. If that
  gate ever misses a frame, the edit falls back to the scene diff rather than being lost.

`tiles.create` was the last MCP straggler: binding the `TileMapComponent` is its only scene
mutation (the tileset/tilemap are files), so it now records a `SetComponentCommand`.

### Deliberately not undoable

Not gaps — these mutate something other than scene state, and the old diff recorded nothing
for them either:

- File writes: `scene.save`, `scene.apply_prefab_instance`, `atlas.slice`, `animation.create`,
  `tiles.add_layer`, and the asset half of `tiles.create`.
- Editor view state: `editor.camera`, window/layout toggles, selection.
- The Scene Transient toggle — an editor-only serialization marker.

### Outstanding

**Runtime verification.** Everything is compile- and unit-tested (292 cases), but the
interactive behaviour has never been exercised in a running editor. With the net gone, a
missed recording is now a silent loss of undo rather than a fallback to the diff. Worth
checking by hand: one Ctrl+Z per gizmo drag; per inspector slider drag; per multi-select
drag; hierarchy drop returning to its exact sibling slot; script/tag edits; and that
play/stop still leaves history sane.

## Build/Test Commands

```powershell
cmake --build --preset default --target Editor
ctest --test-dir build/default -C RelWithDebInfo --output-on-failure
# Or full gauntlet via MCP run_gauntlet
```

## Key Constraints

- Render thread never touches ECS
- Bindless textures bind at set 0
- `GraphicsDevice` member declaration order = destruction order
- Engine init/shutdown order is dependency-sensitive
- `AetherCore.Interop` stays runtime-safe (no editor dependencies)
- Scene `.toml` transforms are world-space
- `std::unique_ptr` for ownership; GPU wrappers are move-only
