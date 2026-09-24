// Regression tests for the editor undo/redo command logic. Every edit is a typed
// command recorded at the point of mutation - there is no scene-snapshot fallback -
// so these exercise the commands directly against a World (no editor, no GPU) and
// lock in the invariants that were previously only checked by hand through the MCP
// control endpoints:
//   - each command restores exactly what it captured, in place, preserving ids
//   - hierarchy moves restore the original sibling index, not just the parent
//   - UndoStack ordering, redo-invalidation, and the coalescing of a multi-frame
//     (and multi-entity) inspector drag into one command per touched component

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "debug/EditorCommand.hpp"
#include "scene/reflection/Reflection.hpp"
#include "assets/TileAssetStore.hpp"
#include "editor/UndoStack.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;
using namespace aether::editor;

namespace
{
	std::string NameOf(World& world, Entity entity)
	{
		const auto* name = world.TryGet<NameComponent>(entity);
		return name != nullptr ? name->name : std::string{};
	}

	glm::vec3 PosOf(World& world, Entity entity)
	{
		const auto* transform = world.TryGet<TransformComponent>(entity);
		return transform != nullptr ? glm::vec3(transform->localToWorld[3]) : glm::vec3(0.0f);
	}

	Entity MakeEntity(World& world, std::string name, glm::vec3 pos)
	{
		const Entity entity = world.Create();
		world.Emplace<NameComponent>(entity, NameComponent{.name = std::move(name)});
		world.Emplace<TransformComponent>(entity, TransformComponent{glm::translate(glm::mat4(1.0f), pos)});
		return entity;
	}

} // namespace

TEST_CASE("TransformCommand restores exactly the affected entity's world matrix")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(1.0f, 2.0f, 3.0f));

	const glm::mat4 before = world.TryGet<TransformComponent>(entity)->localToWorld;
	const glm::mat4 after = glm::translate(glm::mat4(1.0f), glm::vec3(9.0f, 9.0f, 9.0f));
	TransformCommand command({{entity.id, before, after}});

	command.Redo(world, services);
	CHECK(PosOf(world, entity).x == doctest::Approx(9.0f));
	command.Undo(world, services);
	CHECK(PosOf(world, entity).x == doctest::Approx(1.0f));
	CHECK(PosOf(world, entity).z == doctest::Approx(3.0f));
}

TEST_CASE("UndoStack ordering and redo invalidation")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	const glm::mat4 origin = world.TryGet<TransformComponent>(entity)->localToWorld;
	const glm::mat4 one = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	const glm::mat4 two = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));

	UndoStack stack;
	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, origin, one}}));
	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, one, two}}));
	CHECK(stack.UndoDepth() == 2);
	CHECK(stack.RedoDepth() == 0);

	CHECK(stack.Undo(world, services) != nullptr); // reverses the second command (two -> one)
	CHECK(stack.UndoDepth() == 1);
	CHECK(stack.RedoDepth() == 1);
	CHECK(PosOf(world, entity).x == doctest::Approx(1.0f));

	// A fresh command clears the redo stack.
	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, one, two}}));
	CHECK(stack.RedoDepth() == 0);
	CHECK(stack.UndoDepth() == 2);
}

TEST_CASE("UndoStack tracks unsaved changes across edits, save, undo and load")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	const glm::mat4 origin = world.TryGet<TransformComponent>(entity)->localToWorld;
	const glm::mat4 one = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	UndoStack stack;
	CHECK_FALSE(stack.HasUnsavedChanges()); // fresh stack is clean

	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, origin, one}}));
	CHECK(stack.HasUnsavedChanges()); // an edit makes it dirty

	stack.MarkSaved();
	CHECK_FALSE(stack.HasUnsavedChanges()); // saving pins the clean point

	// Undoing after a save moves AWAY from what was written, so it is an outstanding
	// change - the scene no longer matches the file.
	CHECK(stack.Undo(world, services) != nullptr);
	CHECK(stack.HasUnsavedChanges());

	stack.MarkSaved();
	CHECK_FALSE(stack.HasUnsavedChanges());

	// A scene load (Clear) resets to clean.
	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, origin, one}}));
	CHECK(stack.HasUnsavedChanges());
	stack.Clear();
	CHECK_FALSE(stack.HasUnsavedChanges());

	// A save is pinned optimistically because the write happens on another thread, and
	// the pin has to be droppable when that write turns out to have failed - otherwise
	// the editor keeps claiming the scene matches a file that was never written.
	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, origin, one}}));
	stack.MarkSaved();
	CHECK_FALSE(stack.HasUnsavedChanges());
	stack.MarkUnsaved();
	CHECK(stack.HasUnsavedChanges());

	// And it stays dirty until something actually saves: a failed write must not be
	// papered over by an unrelated undo returning to the same depth.
	CHECK(stack.Undo(world, services) != nullptr);
	CHECK(stack.HasUnsavedChanges());
}

TEST_CASE("RenameCommand restores the old name on undo and reapplies on redo")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "Original", glm::vec3(0.0f));

	RenameCommand command(entity.id, "Original", "Renamed");
	command.Redo(world, services);
	CHECK(NameOf(world, entity) == "Renamed");
	command.Undo(world, services);
	CHECK(NameOf(world, entity) == "Original");
}

TEST_CASE("ReparentCommand restores the previous parent on undo")
{
	World world;
	ServiceContainer services;
	const Entity parentA = MakeEntity(world, "ParentA", glm::vec3(0.0f));
	const Entity parentB = MakeEntity(world, "ParentB", glm::vec3(1.0f, 0.0f, 0.0f));
	const Entity child = MakeEntity(world, "Child", glm::vec3(2.0f, 0.0f, 0.0f));
	ecs::SetParent(world, child, parentA);

	// The edit reparents child from A to B; the command records that transition.
	ecs::SetParent(world, child, parentB);
	ReparentCommand command(child.id, parentA.id, parentB.id);

	command.Undo(world, services);
	CHECK(world.TryGet<HierarchyComponent>(child)->parent == parentA);
	command.Redo(world, services);
	CHECK(world.TryGet<HierarchyComponent>(child)->parent == parentB);
}

TEST_CASE("AddScriptCommand removes the script on undo and re-adds it on redo")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	auto& sc = world.Emplace<ScriptComponent>(entity);
	ScriptEntry entry;
	entry.path = "Player";
	sc.scripts.push_back(entry);

	AddScriptCommand command(entity.id, "Player", entry);
	command.Undo(world, services);
	CHECK(world.TryGet<ScriptComponent>(entity)->scripts.empty());
	command.Redo(world, services);
	REQUIRE(world.TryGet<ScriptComponent>(entity)->scripts.size() == 1);
	CHECK(world.TryGet<ScriptComponent>(entity)->scripts[0].path == "Player");
}

// Both script commands used EmplaceOrReplace, which default-constructs: restoring one
// script threw away every other script on the entity. The existing cases above only ever
// had a single script, so neither of them could see it.
TEST_CASE("RemoveScriptCommand undo keeps the entity's other scripts")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	auto& sc = world.Emplace<ScriptComponent>(entity);
	ScriptEntry keep;
	keep.path = "Keep";
	sc.scripts.push_back(keep);

	ScriptEntry removed;
	removed.path = "Removed";
	RemoveScriptCommand command(entity.id, "Removed", removed);
	command.Undo(world, services);

	const auto& scripts = world.TryGet<ScriptComponent>(entity)->scripts;
	REQUIRE(scripts.size() == 2);
	CHECK(std::ranges::any_of(scripts, [](const ScriptEntry& e) { return e.path == "Keep"; }));
	CHECK(std::ranges::any_of(scripts, [](const ScriptEntry& e) { return e.path == "Removed"; }));
}

TEST_CASE("AddScriptCommand redo keeps the entity's other scripts")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	auto& sc = world.Emplace<ScriptComponent>(entity);
	ScriptEntry keep;
	keep.path = "Keep";
	sc.scripts.push_back(keep);

	ScriptEntry added;
	added.path = "Added";
	AddScriptCommand command(entity.id, "Added", added);
	command.Redo(world, services);

	const auto& scripts = world.TryGet<ScriptComponent>(entity)->scripts;
	REQUIRE(scripts.size() == 2);
	CHECK(std::ranges::any_of(scripts, [](const ScriptEntry& e) { return e.path == "Keep"; }));
	CHECK(std::ranges::any_of(scripts, [](const ScriptEntry& e) { return e.path == "Added"; }));
}

TEST_CASE("RemoveScriptCommand re-adds the script on undo and erases it on redo")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	world.Emplace<ScriptComponent>(entity);
	ScriptEntry entry;
	entry.path = "Enemy";

	RemoveScriptCommand command(entity.id, "Enemy", entry);
	command.Undo(world, services);
	REQUIRE(world.TryGet<ScriptComponent>(entity)->scripts.size() == 1);
	CHECK(world.TryGet<ScriptComponent>(entity)->scripts[0].path == "Enemy");
	command.Redo(world, services);
	CHECK(world.TryGet<ScriptComponent>(entity)->scripts.empty());
}

TEST_CASE("SetScriptsCommand restores the whole script list and forces re-attach")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	auto& sc = world.Emplace<ScriptComponent>(entity);
	ScriptEntry player;
	player.path = "Player";
	sc.scripts.push_back(player);

	const std::vector<ScriptEntry> before = sc.scripts;
	ScriptEntry enemy;
	enemy.path = "Enemy";
	sc.scripts.push_back(enemy);
	sc.scripts[0].attached = true;
	const std::vector<ScriptEntry> after = sc.scripts;

	SetScriptsCommand command(entity.id, before, after);
	command.Undo(world, services);
	REQUIRE(world.TryGet<ScriptComponent>(entity)->scripts.size() == 1);
	CHECK(world.TryGet<ScriptComponent>(entity)->scripts[0].path == "Player");
	// The managed instance is gone, so the restored entry must re-attach.
	CHECK_FALSE(world.TryGet<ScriptComponent>(entity)->scripts[0].attached);

	command.Redo(world, services);
	REQUIRE(world.TryGet<ScriptComponent>(entity)->scripts.size() == 2);
	CHECK(world.TryGet<ScriptComponent>(entity)->scripts[1].path == "Enemy");
}

TEST_CASE("ScriptListsEqual ignores the runtime attached flag but sees authored edits")
{
	std::vector<ScriptEntry> a(1);
	a[0].path = "Player";
	std::vector<ScriptEntry> b = a;

	b[0].attached = true; // flipped by the script system, not a user edit
	CHECK(ScriptListsEqual(a, b));

	b[0].path = "Enemy";
	CHECK_FALSE(ScriptListsEqual(a, b));
}

TEST_CASE("HierarchyMoveCommand restores both parent and sibling index on undo")
{
	World world;
	ServiceContainer services;
	const Entity parentA = MakeEntity(world, "A", glm::vec3(0.0f));
	const Entity parentB = MakeEntity(world, "B", glm::vec3(0.0f));
	const Entity first = MakeEntity(world, "First", glm::vec3(0.0f));
	const Entity mover = MakeEntity(world, "Mover", glm::vec3(0.0f));
	const Entity last = MakeEntity(world, "Last", glm::vec3(0.0f));
	ecs::SetParent(world, first, parentA);
	ecs::SetParent(world, mover, parentA); // index 1 under A
	ecs::SetParent(world, last, parentA);

	// The edit moves it under B; the command records that slot transition.
	ecs::InsertChildAt(world, mover, parentB, 0);
	HierarchyMoveCommand command(std::vector<HierarchyMoveCommand::Item>{{mover.id, parentA.id, 1, parentB.id, 0}});

	command.Undo(world, services);
	REQUIRE(world.TryGet<HierarchyComponent>(mover) != nullptr);
	CHECK(world.TryGet<HierarchyComponent>(mover)->parent == parentA);
	const auto& restored = world.TryGet<HierarchyComponent>(parentA)->children;
	REQUIRE(restored.size() == 3);
	CHECK(restored[1] == mover); // back in its original slot, not just its original parent

	command.Redo(world, services);
	CHECK(world.TryGet<HierarchyComponent>(mover)->parent == parentB);
}

TEST_CASE("UndoStack coalesces an inspector field drag into a single command")
{
	UndoStack stack;
	// A drag fires every frame with the intermediate values.
	stack.RecordFieldEdit(7, "Point Light", "intensity", 1.0, 2.0, true);
	stack.RecordFieldEdit(7, "Point Light", "intensity", 2.0, 3.0, true);
	stack.RecordFieldEdit(7, "Point Light", "intensity", 3.0, 4.0, true);
	CHECK(stack.UndoDepth() == 0); // nothing lands until the interaction ends

	stack.FlushFieldEdit();
	CHECK(stack.UndoDepth() == 1); // the whole drag is one undo step
}

TEST_CASE("Recording a command finalizes an in-flight field edit instead of dropping it")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	const glm::mat4 origin = world.TryGet<TransformComponent>(entity)->localToWorld;
	const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	UndoStack stack;
	stack.RecordFieldEdit(entity.id, "Point Light", "intensity", 1.0, 2.0, true);
	// An unrelated command lands mid-drag (e.g. deleting something); the drag must
	// be finalized ahead of it, not swallowed.
	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, origin, moved}}));
	CHECK(stack.UndoDepth() == 2);

	// Undo order proves the field edit was recorded first.
	CHECK(stack.Undo(world, services) != nullptr);
	CHECK(PosOf(world, entity).x == doctest::Approx(0.0f));
}

TEST_CASE("UndoStack drops a field edit that ends where it started")
{
	UndoStack stack;
	stack.RecordFieldEdit(7, "Point Light", "intensity", 1.0, 2.0, true);
	stack.RecordFieldEdit(7, "Point Light", "intensity", 2.0, 1.0, true);
	stack.FlushFieldEdit();
	CHECK(stack.UndoDepth() == 0);
}

TEST_CASE("UndoStack keeps separate components pending until the interaction ends")
{
	UndoStack stack;
	stack.RecordFieldEdit(7, "Point Light", "intensity", 1.0, 2.0, true);
	stack.RecordFieldEdit(7, "Camera", "fov", 60.0, 70.0, true);
	CHECK(stack.UndoDepth() == 0); // still in flight

	stack.FlushFieldEdit();
	// Everything the one interaction touched lands as a single composite entry, so
	// undoing it cannot leave half of the edit applied.
	CHECK(stack.UndoDepth() == 1);
}

TEST_CASE("UndoStack turns one multi-selection drag into one undo entry")
{
	UndoStack stack;
	// One transform drag across a 3-entity selection: every frame touches all three.
	for (int frame = 0; frame < 4; ++frame)
	{
		const double from = 1.0 + frame;
		const double to = 2.0 + frame;
		stack.RecordFieldEdit(1, "Transform", "position", from, to, true);
		stack.RecordFieldEdit(2, "Transform", "position", from, to, true);
		stack.RecordFieldEdit(3, "Transform", "position", from, to, true);
	}
	CHECK(stack.UndoDepth() == 0); // nothing lands mid-drag

	stack.FlushFieldEdit();
	// Not one per frame (that was the point of coalescing) and not one per entity
	// either: the drag was a single gesture, so it costs a single Ctrl+Z. Recording
	// it per entity left the selection visibly half-reverted between presses.
	CHECK(stack.UndoDepth() == 1);
}

TEST_CASE("UndoStack undoes a whole multi-selection edit in one step")
{
	World world;
	ServiceContainer services;
	const Entity a = world.Create();
	const Entity b = world.Create();
	world.Emplace<NameComponent>(a, NameComponent{"A"});
	world.Emplace<NameComponent>(b, NameComponent{"B"});
	world.Emplace<PointLightComponent>(a, PointLightComponent{});
	world.Emplace<PointLightComponent>(b, PointLightComponent{});
	world.Get<PointLightComponent>(a).intensity = 5.0f;
	world.Get<PointLightComponent>(b).intensity = 5.0f;

	UndoStack stack;
	stack.RecordFieldEdit(a.id, "Point Light", "intensity", 1.0, 5.0, true);
	stack.RecordFieldEdit(b.id, "Point Light", "intensity", 1.0, 5.0, true);
	stack.FlushFieldEdit();
	REQUIRE(stack.UndoDepth() == 1);

	REQUIRE(stack.Undo(world, services) != nullptr);
	// Both entities revert together - the failure this guards against is one of them
	// staying at the edited value until a second undo.
	CHECK(world.Get<PointLightComponent>(a).intensity == doctest::Approx(1.0f));
	CHECK(world.Get<PointLightComponent>(b).intensity == doctest::Approx(1.0f));
	CHECK(stack.UndoDepth() == 0);
}

TEST_CASE("UnpackPrefabCommand re-links the prefab instance in place on undo")
{
	World world;
	ServiceContainer services;
	const Entity root = MakeEntity(world, "InstanceRoot", glm::vec3(0.0f));
	const Entity child = MakeEntity(world, "InstanceChild", glm::vec3(1.0f, 0.0f, 0.0f));
	ecs::SetParent(world, child, root);
	world.Emplace<PrefabInstanceComponent>(root, PrefabInstanceComponent{.prefabPath = "prefabs/Hero.prefab"});
	world.Emplace<SceneTransientComponent>(root);
	world.Emplace<PrefabLinkComponent>(child, PrefabLinkComponent{.instanceRoot = root, .prefabGuid = 42});
	world.Emplace<SceneTransientComponent>(child);

	// Snapshot before the unpack strips the linkage (mirrors the control handler).
	auto command = UnpackPrefabCommand::Capture(world, root);
	REQUIRE(command != nullptr);
	for (const Entity e: {root, child})
	{
		world.Remove<PrefabLinkComponent>(e);
		world.Remove<SceneTransientComponent>(e);
	}
	world.Remove<PrefabInstanceComponent>(root);

	command->Undo(world, services);
	REQUIRE(world.Has<PrefabInstanceComponent>(root));
	CHECK(world.TryGet<PrefabInstanceComponent>(root)->prefabPath == "prefabs/Hero.prefab");
	CHECK(world.Has<SceneTransientComponent>(root));
	REQUIRE(world.Has<PrefabLinkComponent>(child));
	CHECK(world.TryGet<PrefabLinkComponent>(child)->prefabGuid == 42);
	CHECK(world.TryGet<PrefabLinkComponent>(child)->instanceRoot == root);
	CHECK(world.Has<SceneTransientComponent>(child));

	command->Redo(world, services);
	CHECK_FALSE(world.Has<PrefabInstanceComponent>(root));
	CHECK_FALSE(world.Has<PrefabLinkComponent>(child));
	CHECK_FALSE(world.Has<SceneTransientComponent>(child));
}

// ── Undoing back to the saved state is clean ────────────────────────────────
//
// This tracked a COUNT of operations, so undo looked like another edit and a scene
// edited once then undone read as dirty forever. That had autosave writing recovery
// copies holding no work, and the recovery prompt offering them on every project open.

namespace
{
	// One recorded edit, applied. Content does not matter here - only the shape of the
	// history the clean marker moves through.
	std::unique_ptr<IEditorCommand> AnEdit(Entity entity, const glm::mat4& from, const glm::mat4& to)
	{
		return std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, from, to}});
	}
} // namespace

TEST_CASE("Undoing back to the saved state reports clean again")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	const glm::mat4 origin = world.TryGet<TransformComponent>(entity)->localToWorld;
	const glm::mat4 one = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	UndoStack stack;
	stack.MarkSaved(); // the scene as loaded, matching the file

	stack.Record(AnEdit(entity, origin, one));
	CHECK(stack.HasUnsavedChanges());

	// Back to exactly what the file holds. Nothing to save, nothing to recover.
	CHECK(stack.Undo(world, services) != nullptr);
	CHECK_FALSE(stack.HasUnsavedChanges());

	// And forward again.
	CHECK(stack.Redo(world, services) != nullptr);
	CHECK(stack.HasUnsavedChanges());
}

TEST_CASE("Undo then a different edit is never reported clean")
{
	// The failure a position-based marker has to defend against: the depth returns to
	// where the save happened while the content is something else entirely.
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	const glm::mat4 origin = world.TryGet<TransformComponent>(entity)->localToWorld;
	const glm::mat4 one = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
	const glm::mat4 two = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));

	UndoStack stack;
	stack.Record(AnEdit(entity, origin, one));
	stack.MarkSaved(); // saved at depth 1
	CHECK_FALSE(stack.HasUnsavedChanges());

	CHECK(stack.Undo(world, services) != nullptr); // depth 0
	stack.Record(AnEdit(entity, origin, two));     // depth 1 again, different content

	CHECK(stack.UndoDepth() == 1); // same depth the save was pinned at
	CHECK(stack.HasUnsavedChanges());

	// It must stay dirty: the saved state is behind a discarded branch, so no amount of
	// undoing gets back to it.
	CHECK(stack.Undo(world, services) != nullptr);
	CHECK(stack.HasUnsavedChanges());
}

TEST_CASE("A saved state that ages out of the ring stops being reachable")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	const glm::mat4 origin = world.TryGet<TransformComponent>(entity)->localToWorld;
	const glm::mat4 one = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));

	UndoStack stack;
	stack.MarkSaved(); // clean at depth 0

	// Fill past the ring so the command that depth 0 sat before is discarded.
	for (std::size_t i = 0; i <= UndoStack::kMaxDepth; ++i)
	{
		stack.Record(AnEdit(entity, origin, one));
	}
	CHECK(stack.UndoDepth() == UndoStack::kMaxDepth);
	CHECK(stack.HasUnsavedChanges());

	// Undoing everything still available cannot reach the saved state, so it must not
	// claim to have.
	while (stack.UndoDepth() > 0)
	{
		CHECK(stack.Undo(world, services) != nullptr);
	}
	CHECK(stack.HasUnsavedChanges());
}

TEST_CASE("UndoStack groups a batch into one history entry")
{
	World world;
	ServiceContainer services;
	const Entity a = world.Create();
	const Entity b = world.Create();
	world.Emplace<NameComponent>(a, NameComponent{"A"});
	world.Emplace<NameComponent>(b, NameComponent{"B"});

	UndoStack stack;
	{
		UndoStack::ScopedGroup group(&stack, "Batch edit");
		stack.Record(std::make_unique<RenameCommand>(a.id, "A", "A2"));
		stack.Record(std::make_unique<RenameCommand>(b.id, "B", "B2"));
		CHECK(stack.UndoDepth() == 0); // nothing lands until the group closes
	}
	REQUIRE(stack.UndoDepth() == 1);

	world.Get<NameComponent>(a).name = "A2";
	world.Get<NameComponent>(b).name = "B2";
	REQUIRE(stack.Undo(world, services) != nullptr);
	CHECK(world.Get<NameComponent>(a).name == "A");
	CHECK(world.Get<NameComponent>(b).name == "B");
	CHECK(stack.UndoDepth() == 0);
}

// Deleting a tilemap layer takes every tile painted on it. It recorded nothing at all, so
// the delete was permanent - Ctrl+Z reached past it into an unrelated edit.
TEST_CASE("RemoveTileLayerCommand restores the layer and its tiles")
{
	const std::string mapPath = (std::filesystem::temp_directory_path() / "aether_undo_tilelayer.atlm").generic_string();
	TileMapAsset seed;
	seed.layers.emplace_back();
	seed.layers.emplace_back();
	seed.layers[1].name = "Painted";
	seed.SetCell(1, {4, 7}, tilecell::Make(3));

	TileAssetStore store;
	REQUIRE(store.SaveTileMap(mapPath, seed).has_value());

	ServiceContainer services;
	services.Register<TileAssetStore>(store);
	World world;

	TileMapAsset* live = store.MutableTileMap(mapPath);
	REQUIRE(live != nullptr);
	REQUIRE(live->layers.size() == 2);

	RemoveTileLayerCommand command(mapPath, 1, live->layers[1]);
	command.Redo(world, services);
	CHECK(store.MutableTileMap(mapPath)->layers.size() == 1);

	command.Undo(world, services);
	TileMapAsset* restored = store.MutableTileMap(mapPath);
	REQUIRE(restored->layers.size() == 2);
	CHECK(restored->layers[1].name == "Painted");
	// The tiles have to come back with it, not just an empty layer.
	CHECK(restored->GetCell(1, {4, 7}) == tilecell::Make(3));

	std::filesystem::remove(mapPath);
}

// Removing an entity's effect used to record nothing, so Ctrl+Z skipped past it and the
// tuning was gone. Only the destructive half is exercised here: restoring goes through
// ApplyEntityEffect, which needs a live EffectManager, PipelineCache and param buffer.
TEST_CASE("RemoveEffectCommand strips the effect components on redo")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	world.Emplace<EffectRefComponent>(entity, EffectRefComponent{.name = "molten"});
	world.Emplace<EffectParamsComponent>(entity);
	world.Emplace<PipelineComponent>(entity);

	RemoveEffectCommand command(entity.id, "molten", EffectParams{});
	command.Redo(world, services);

	CHECK_FALSE(world.Has<EffectRefComponent>(entity));
	CHECK_FALSE(world.Has<EffectParamsComponent>(entity));
	// With no AssetManager there is no material to fall back to, so the pipeline goes too
	// rather than being left pointing at the effect's.
	CHECK_FALSE(world.Has<PipelineComponent>(entity));
}

// Undo with no services must decline rather than half-restore: a component put back without
// its parameter slot or pipeline is worse than leaving the removal in place.
TEST_CASE("RemoveEffectCommand undo declines when the effect services are absent")
{
	World world;
	ServiceContainer services;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));

	RemoveEffectCommand command(entity.id, "molten", EffectParams{});
	command.Undo(world, services);

	CHECK_FALSE(world.Has<EffectRefComponent>(entity));
	CHECK_FALSE(world.Has<EffectParamsComponent>(entity));
}

// Undo re-creates a removed component by looking its name up in the component CATALOG, which
// is a UI menu rather than a registry: colliders appear in it under shape-specific names, so
// "Collider" found nothing, the component was never re-created, and ApplyComponentFields
// gives up on a component that does not exist. Undo silently restored nothing.
//
// The fallback is the reflected type's emplaceDefault, so every reflected component the
// editor can remove has to actually have one.
TEST_CASE("Every reflected component can be re-created, so its removal is undoable")
{
	const auto& types = reflect::ComponentTypes();
	REQUIRE_FALSE(types.empty());

	std::vector<std::string> missing;
	for (const reflect::ComponentType& type: types)
	{
		// Reference-only types are never added to an arbitrary entity, so they are never
		// removed from one either.
		if (type.addable && !type.emplaceDefault)
		{
			missing.push_back(type.name);
		}
	}
	INFO("components that undo could not re-create: " << missing.size());
	CHECK(missing.empty());
}

// The specific name that was broken. Guards against the catalog gaining a "Collider" entry
// (or the reflected name changing) and quietly diverging again.
TEST_CASE("The reflected Collider can be re-created by name")
{
	const reflect::ComponentType* type = reflect::FindComponentType("Collider");
	REQUIRE(type != nullptr);
	CHECK(type->emplaceDefault);

	World world;
	const Entity entity = MakeEntity(world, "E", glm::vec3(0.0f));
	REQUIRE_FALSE(world.Has<ColliderComponent>(entity));

	(void) type->emplaceDefault(world, entity);
	CHECK(world.Has<ColliderComponent>(entity));
}

TEST_CASE("UndoStack group of one records the command itself")
{
	UndoStack stack;
	{
		UndoStack::ScopedGroup group(&stack, "Batch edit");
		stack.Record(std::make_unique<RenameCommand>(1, "x", "y"));
	}
	// No composite wrapper for a single command - one entry either way, but the history
	// should read as the edit that happened, not as a batch of one.
	REQUIRE(stack.UndoDepth() == 1);
}

TEST_CASE("UndoStack ignores a field diff for a component a command already recorded")
{
	// The Inspector diffs every drawn component after drawing to catch multi-frame widget
	// drags. That pass runs on the same frame a context-menu command mutates a component -
	// a paste, a reset - because the click that fires the menu item is a mouse release.
	// Without the claim the change lands twice: once as the command, once as a drag nobody
	// made, and one Ctrl+Z only half-undoes it.
	UndoStack stack;
	stack.Record(std::make_unique<SetComponentCommand>(7, "Collider", nlohmann::json{{"radius", 1.0}}, nlohmann::json{{"radius", 2.0}}, /*isReflected=*/true));
	REQUIRE(stack.UndoDepth() == 1);

	stack.RecordFieldEdit(7, "Collider", "radius", nlohmann::json(1.0), nlohmann::json(2.0), /*isReflected=*/true);
	stack.FlushFieldEdit();
	CHECK(stack.UndoDepth() == 1);

	// A different component on the same entity was never claimed, so its edit still counts.
	stack.RecordFieldEdit(7, "Rigid Body", "mass", nlohmann::json(1.0), nlohmann::json(2.0), /*isReflected=*/true);
	stack.FlushFieldEdit();
	CHECK(stack.UndoDepth() == 2);
}

TEST_CASE("UndoStack edit claims last only until cleared")
{
	// Claims live from one frame's drawing to that frame's diff. A drag on the next frame
	// is a real edit and must still be recorded.
	UndoStack stack;
	stack.Record(std::make_unique<SetComponentCommand>(7, "Collider", nlohmann::json{{"radius", 1.0}}, nlohmann::json{{"radius", 2.0}}, /*isReflected=*/true));
	stack.ClearEditClaims();
	stack.RecordFieldEdit(7, "Collider", "radius", nlohmann::json(2.0), nlohmann::json(3.0), /*isReflected=*/true);
	stack.FlushFieldEdit();
	CHECK(stack.UndoDepth() == 2);
}

TEST_CASE("UndoStack empty group leaves no entry")
{
	UndoStack stack;
	{
		UndoStack::ScopedGroup group(&stack, "Batch edit");
	}
	CHECK(stack.UndoDepth() == 0);
}

TEST_CASE("UndoStack nested groups emit once")
{
	UndoStack stack;
	{
		UndoStack::ScopedGroup outer(&stack, "Outer");
		stack.Record(std::make_unique<RenameCommand>(1, "a", "b"));
		{
			UndoStack::ScopedGroup inner(&stack, "Inner");
			stack.Record(std::make_unique<RenameCommand>(2, "c", "d"));
			CHECK(stack.UndoDepth() == 0);
		}
		CHECK(stack.UndoDepth() == 0); // the inner close must not emit
	}
	CHECK(stack.UndoDepth() == 1);
}
