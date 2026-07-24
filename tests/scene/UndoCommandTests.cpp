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
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "debug/EditorCommand.hpp"
#include "debug/UndoStack.hpp"
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

	// Undoing after a save still counts as an outstanding change (conservative: never
	// reports clean while the on-disk state differs from what a save just wrote).
	CHECK(stack.Undo(world, services) != nullptr);
	CHECK(stack.HasUnsavedChanges());

	stack.MarkSaved();
	CHECK_FALSE(stack.HasUnsavedChanges());

	// A scene load (Clear) resets to clean.
	stack.Record(std::make_unique<TransformCommand>(std::vector<TransformCommand::Item>{{entity.id, origin, one}}));
	CHECK(stack.HasUnsavedChanges());
	stack.Clear();
	CHECK_FALSE(stack.HasUnsavedChanges());
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
	CHECK(stack.UndoDepth() == 2); // one command per touched component
}

TEST_CASE("UndoStack coalesces a multi-selection drag per entity, not per frame")
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
	CHECK(stack.UndoDepth() == 3); // one command per entity, not one per frame
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
