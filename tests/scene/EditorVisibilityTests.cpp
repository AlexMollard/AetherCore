// The hierarchy's eye toggle set HiddenTag and nothing read it, so hiding an entity changed
// its icon and nothing else. These pin what the toggle now means: it hides in the editor
// only, it is inherited by children, and it does not survive into play or a build.
#include <doctest/doctest.h>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"

namespace
{
	// The flag is a global the editor drives from its play state, so every case sets it
	// explicitly rather than depending on whatever ran before.
	struct VisibilityScope
	{
		explicit VisibilityScope(bool respected) { aether::ecs::EditorSceneVisibilityRespected() = respected; }
		~VisibilityScope() { aether::ecs::EditorSceneVisibilityRespected() = false; }
		VisibilityScope(const VisibilityScope&) = delete;
		VisibilityScope& operator=(const VisibilityScope&) = delete;
	};
} // namespace

TEST_CASE("A hidden entity is hidden in the editor and not in play")
{
	aether::World world;
	const aether::Entity entity = world.Create();
	world.Emplace<aether::HiddenTag>(entity);

	{
		const VisibilityScope editing{true};
		CHECK(aether::ecs::IsHiddenInEditor(world, entity));
	}
	{
		// Play, and a shipped game, never set the flag - so the tag has no effect there even
		// though it is still on the entity.
		const VisibilityScope playing{false};
		CHECK_FALSE(aether::ecs::IsHiddenInEditor(world, entity));
	}
}

TEST_CASE("Hiding an entity hides its children")
{
	const VisibilityScope editing{true};
	aether::World world;
	const aether::Entity parent = world.Create();
	const aether::Entity child = world.Create();
	aether::ecs::SetParent(world, child, parent);

	CHECK_FALSE(aether::ecs::IsHiddenInEditor(world, child));

	world.Emplace<aether::HiddenTag>(parent);
	CHECK(aether::ecs::IsHiddenInEditor(world, parent));
	CHECK(aether::ecs::IsHiddenInEditor(world, child));
}

TEST_CASE("Hiding is independent of disabling")
{
	const VisibilityScope editing{true};
	aether::World world;
	const aether::Entity entity = world.Create();
	world.Emplace<aether::HiddenTag>(entity);

	// Hidden is an editor-view state; disabled is scene content. An entity hidden while
	// authoring is still active in the scene it gets saved into.
	CHECK(aether::ecs::IsActiveInHierarchy(world, entity));
	CHECK(aether::ecs::IsHiddenInEditor(world, entity));
}
