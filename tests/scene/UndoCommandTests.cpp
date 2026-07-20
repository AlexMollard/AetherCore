// Regression tests for the editor undo/redo command logic (command-based history
// that replaced the whole-scene snapshot system). These exercise the commands
// directly against a World - no editor, no GPU - so they lock in the invariants
// that were previously only checked by hand through the MCP control endpoints:
//   - transform undo/redo restores exactly the affected entities
//   - the surgical EntityDiffCommand detects create / delete / edit and only
//     touches those subtrees, preserving ids and (crucially) not orphaning the
//     unchanged children of an edited parent
//   - UndoStack ordering + redo-invalidation-on-new-command

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

#include "debug/EditorCommand.hpp"
#include "debug/UndoStack.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;
using namespace aether::editor;
using aether::app::scene::CaptureScene;
using aether::app::scene::SceneDescription;

namespace
{
	bool Alive(World& world, Entity entity)
	{
		return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
	}

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

	// A capture context (standalone registries; no AssetManager/GPU needed).
	struct CaptureCtx
	{
		FakeSlotSink sink{16};
		FakeTextureSink tsink{};
		TextureRegistry treg{tsink};
		MaterialRegistry mreg{sink, treg};

		SceneDescription Snap(World& world)
		{
			return CaptureScene(world, mreg, treg);
		}
	};
}

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

TEST_CASE("EntityDiffCommand: edit reverts and redoes, other entities untouched")
{
	World world;
	ServiceContainer services;
	CaptureCtx ctx;

	const Entity a = MakeEntity(world, "Alpha", glm::vec3(0.0f));
	const Entity b = MakeEntity(world, "Beta", glm::vec3(5.0f, 0.0f, 0.0f));

	const SceneDescription before = ctx.Snap(world);
	world.Get<NameComponent>(a).name = "AlphaRenamed";
	const SceneDescription after = ctx.Snap(world);

	EntityDiffCommand command(before, after);
	CHECK_FALSE(command.Empty());

	command.Undo(world, services);
	CHECK(NameOf(world, a) == "Alpha");
	CHECK(NameOf(world, b) == "Beta"); // untouched
	CHECK(Alive(world, b));

	command.Redo(world, services);
	CHECK(NameOf(world, a) == "AlphaRenamed");
}

TEST_CASE("EntityDiffCommand: create is removed on undo and restored on redo")
{
	World world;
	ServiceContainer services;
	CaptureCtx ctx;

	const Entity a = MakeEntity(world, "Keep", glm::vec3(0.0f));
	const SceneDescription before = ctx.Snap(world);
	const Entity b = MakeEntity(world, "New", glm::vec3(1.0f, 1.0f, 0.0f));
	const SceneDescription after = ctx.Snap(world);

	EntityDiffCommand command(before, after);
	command.Undo(world, services);
	CHECK(Alive(world, a));
	CHECK_FALSE(Alive(world, b)); // the created entity is gone

	command.Redo(world, services);
	CHECK(Alive(world, a));
	// Recreated under the same id (its slot was free), so the original handle is valid again.
	CHECK(Alive(world, b));
	CHECK(NameOf(world, b) == "New");
}

TEST_CASE("EntityDiffCommand: delete is restored (same id) on undo")
{
	World world;
	ServiceContainer services;
	CaptureCtx ctx;

	const Entity a = MakeEntity(world, "Alpha", glm::vec3(0.0f));
	const Entity b = MakeEntity(world, "Beta", glm::vec3(7.0f, 0.0f, 0.0f));
	const SceneDescription before = ctx.Snap(world);
	ecs::DestroyHierarchy(world, b);
	const SceneDescription after = ctx.Snap(world);

	EntityDiffCommand command(before, after);
	command.Undo(world, services);
	CHECK(Alive(world, a));
	CHECK(Alive(world, b)); // restored under the same id
	CHECK(NameOf(world, b) == "Beta");
	CHECK(PosOf(world, b).x == doctest::Approx(7.0f));

	command.Redo(world, services);
	CHECK_FALSE(Alive(world, b));
}

TEST_CASE("EntityDiffCommand: editing a parent does not orphan its children")
{
	World world;
	ServiceContainer services;
	CaptureCtx ctx;

	const Entity parent = MakeEntity(world, "Parent", glm::vec3(0.0f));
	const Entity child = MakeEntity(world, "Child", glm::vec3(1.0f, 0.0f, 0.0f));
	ecs::SetParent(world, child, parent);

	const SceneDescription before = ctx.Snap(world);
	world.Get<NameComponent>(parent).name = "ParentEdited";
	const SceneDescription after = ctx.Snap(world);

	EntityDiffCommand command(before, after);
	command.Undo(world, services);

	CHECK(NameOf(world, parent) == "Parent");
	REQUIRE(Alive(world, child));
	const auto* childHierarchy = world.TryGet<HierarchyComponent>(child);
	REQUIRE(childHierarchy != nullptr);
	CHECK(childHierarchy->parent == parent); // still parented, not orphaned
	const auto* parentHierarchy = world.TryGet<HierarchyComponent>(parent);
	REQUIRE(parentHierarchy != nullptr);
	CHECK(std::find(parentHierarchy->children.begin(), parentHierarchy->children.end(), child) != parentHierarchy->children.end());
}

TEST_CASE("EntityDiffCommand ignores animator-driven sprite frame changes")
{
	World world;
	CaptureCtx ctx;
	const Entity e = MakeEntity(world, "Anim", glm::vec3(0.0f));
	world.Emplace<SpriteRendererComponent>(e, SpriteRendererComponent{});
	world.Emplace<SpriteAnimatorComponent>(e, SpriteAnimatorComponent{});

	const app::scene::SceneDescription before = ctx.Snap(world);
	// Simulate an edit-mode preview advancing the frame (ApplyFrame writes these).
	world.Get<SpriteRendererComponent>(e).uvRect = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
	world.Get<SpriteRendererComponent>(e).pixelSize = glm::vec2(999.0f);
	const app::scene::SceneDescription after = ctx.Snap(world);

	EntityDiffCommand command(before, after);
	CHECK(command.Empty()); // a previewing animation is runtime state, not an authored edit
}

TEST_CASE("EntityDiffCommand still catches authored edits on an animated entity")
{
	World world;
	CaptureCtx ctx;
	const Entity e = MakeEntity(world, "Anim", glm::vec3(0.0f));
	world.Emplace<SpriteRendererComponent>(e, SpriteRendererComponent{});
	world.Emplace<SpriteAnimatorComponent>(e, SpriteAnimatorComponent{});

	const app::scene::SceneDescription before = ctx.Snap(world);
	world.Get<NameComponent>(e).name = "Renamed";
	const app::scene::SceneDescription after = ctx.Snap(world);

	EntityDiffCommand command(before, after);
	CHECK_FALSE(command.Empty()); // renaming is a real edit even with an animator present
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
