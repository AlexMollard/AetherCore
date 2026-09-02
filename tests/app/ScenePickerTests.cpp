// The hierarchy's pointer toggle sets NotPickableTag so you can click through something that
// is in the way. Nothing read the tag, so the toggle did nothing at all. This pins the sprite
// path; the mesh and physics paths honour it through the same predicate.
#include <doctest/doctest.h>

#include "debug/ScenePicker.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

#include <string>

namespace
{
	aether::Entity MakeSprite(aether::World& world, const char* name, float z)
	{
		const aether::Entity e = world.Create();
		world.Emplace<aether::NameComponent>(e, aether::NameComponent{.name = name});
		world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 0.0f, z}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		world.Emplace<aether::SpriteRendererComponent>(e, aether::SpriteRendererComponent{});
		return e;
	}

	// Entity::id carries a version, so an entity rebuilt from the registry does not compare
	// equal to the one Create() handed back. The name is what the test actually means.
	std::string PickedName(aether::World& world)
	{
		const aether::editor::PickHit hit = aether::editor::PickEntity(world, nullptr, aether::Ray{.origin = {0.0f, 0.0f, 10.0f}, .dir = {0.0f, 0.0f, -1.0f}}, 100.0f);
		if (!hit.entity.IsValid())
		{
			return {};
		}
		const auto* name = world.TryGet<aether::NameComponent>(hit.entity);
		return name != nullptr ? name->name : std::string{"<unnamed>"};
	}

} // namespace

TEST_CASE("A sprite marked not pickable is clicked through")
{
	aether::World world;
	const aether::Entity a = MakeSprite(world, "a", 0.0f);
	const aether::Entity b = MakeSprite(world, "b", 1.0f);

	// Which of two identically sorted sprites wins is a tie-break detail this test has no
	// business asserting. What matters is that tagging the winner falls through to the other,
	// and tagging both leaves nothing to click.
	const std::string first = PickedName(world);
	REQUIRE_FALSE(first.empty());

	world.Emplace<aether::NotPickableTag>(first == "a" ? a : b);
	const std::string second = PickedName(world);
	CHECK(second == (first == "a" ? "b" : "a"));

	world.Emplace<aether::NotPickableTag>(first == "a" ? b : a);
	CHECK(PickedName(world).empty());
}
