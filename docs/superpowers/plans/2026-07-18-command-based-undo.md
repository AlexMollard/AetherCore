# Command-Based Editor Undo/Redo Implementation Plan

**Goal:** Replace the whole-scene-snapshot undo system with per-operation reversible commands that preserve entity IDs and selection, are deterministic, and survive play/stop.

**Architecture:** An `IEditorCommand` interface (Undo/Redo/Label). `UndoStack` becomes a command history (two stacks of `unique_ptr<IEditorCommand>`). Most edits are captured as an `EntityStateCommand` that stores before/after serialized state of the *affected entities only* and re-applies it **in place** (reset + `ApplySceneToEntities` onto the same entity handles, never destroy/recreate). Structural ops use `CreateEntityCommand` / `DeleteEntityCommand` (recreate with the same IDs via a new `World::CreateWithId`) and `ReparentCommand`.

**Tech Stack:** C++20, entt, existing scene serialization (`CaptureScene`, `ApplySceneToEntities`, `ResetRestorableEntity`, `MakeApplySceneDeps`).

## Global Constraints
- Never destroy+recreate the whole hierarchy for an undo. Touch only the affected entities.
- Undo/redo must preserve the current selection (prune only genuinely-dead entities).
- `master` must build after every phase; commit + push per phase.
- No editor deps leak into engine `*Exports.cpp` (interop stays runtime-safe).

---

## Phase 1: Command framework + in-place primitive

**Files:**
- Create: `src/app/debug/EditorCommand.hpp` (interface + EntityStateCommand)
- Create: `src/app/debug/EditorCommand.cpp`
- Modify: `src/app/debug/UndoStack.hpp`, `UndoStack.cpp` (command history)
- Modify: `src/engine/scene/World.hpp`, `World.cpp` (`CreateWithId`)
- Modify: `src/app/scene/SceneSerializer.hpp` (expose a subset capture/apply-in-place helper if not already public)

**Design:**
- `IEditorCommand { virtual ~; void Undo(World&, ServiceContainer&); void Redo(...); std::string_view Label(); }`
- `UndoStack`: `Record(unique_ptr<IEditorCommand>)` (push, clear redo), `Undo`, `Redo`, `Clear`, depths. kMaxDepth=64.
- `EntityStateCommand`: holds `std::vector<Entity> targets`, `SceneDescription before`, `SceneDescription after`. Undo → `ApplyInPlace(before)`, Redo → `ApplyInPlace(after)`. `ApplyInPlace` = for each target reset (ResetRestorableEntity) then `ApplySceneToEntities(desc, world, deps, targets)`.
- Helper `CaptureEntities(world, services, span<Entity>) -> SceneDescription` (subset of CaptureScene).
- `World::CreateWithId(Entity)`: `m_registry.create(ToEntt(desired))`, RegisterRoot, return actual (log if it couldn't honor the id).

**Verify:** builds; no call sites changed yet (old Push kept as a thin shim that records an EntityStateCommand of the whole scene so nothing regresses mid-migration). Commit.

## Phase 2: Transform edits (gizmo + drop)

**Files:** `src/app/debug/ViewportPanel.cpp/.hpp`

- Gizmo: on drag begin capture `before` of the selected entities; on drag end record `EntityStateCommand(targets, before, after)`. Remove reliance on the mouse-click snapshot for transforms.
- Sprite-drop (line ~1632) → `CreateEntityCommand`.
- Collider-2D edit (line ~666) → capture before on first edit, record EntityStateCommand on release.

**Verify (editor):** move an entity, Ctrl+Z → it returns AND stays selected. Commit.

## Phase 3: Structural ops (Hierarchy)

**Files:** `src/app/debug/HierarchyPanel.cpp`, `EditorCommand.*`

- `CreateEntityCommand` (create/duplicate), `DeleteEntityCommand` (uses CreateWithId + subtree serialize for undo), `ReparentCommand`.
- Migrate the four Push sites (1830/1851/1890/2028).

**Verify:** create/delete/reparent/duplicate, undo each, IDs + selection intact. Commit.

## Phase 4: Component add/remove/edit (Inspector)

**Files:** `src/app/debug/InspectorPanel.cpp`, `ComponentDrawers.*`

- Component field edits: capture before on edit-begin (IsItemActivated), record EntityStateCommand on edit-commit (IsItemDeactivatedAfterEdit).
- Add/remove component → EntityStateCommand of that entity.

**Verify:** edit a field, undo restores it, entity stays selected. Commit.

## Phase 5: Play/stop integration + remove old system

**Files:** `DebugLayer.cpp`, `UndoStack.*`, `PlaySession.cpp`

- Remove the mouse-click snapshot Push and the `m_selection.Clear()` on undo.
- Command history is independent of the play snapshot, so it survives play→stop; verify the do→play→stop→undo flow.
- Delete the old snapshot-based Push/ReplaceScene undo path and the whole-scene shim from Phase 1.

**Verify:** do→play→stop→undo works; general undo consistent. Commit.
