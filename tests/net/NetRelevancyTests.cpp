#include <doctest/doctest.h>

#include <algorithm>

#include "net/NetRelevancy.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
	Entity MakeNetworked(World& w, glm::vec3 pos, net::ConnectionId owner)
	{
		const Entity e = w.Create();
		w.Emplace<TransformComponent>(e).localToWorld = aether::ComposeTransform(pos, glm::vec3{0.f}, glm::vec3{1.f});
		auto& id = w.Emplace<net::NetworkIdentity>(e);
		id.netId = static_cast<std::uint32_t>(e.id);
		id.owner = owner;
		return e;
	}

	bool Contains(const std::vector<Entity>& v, Entity e)
	{
		return std::find(v.begin(), v.end(), e) != v.end();
	}
} // namespace

TEST_CASE("Only entities inside the radius are relevant")
{
	World w;
	const Entity nearEntity = MakeNetworked(w, {5.f, 0.f, 0.f}, 0);
	const Entity farEntity = MakeNetworked(w, {500.f, 0.f, 0.f}, 0);

	net::RelevancySettings settings;
	settings.radius = 60.f;

	const std::vector<Entity> relevant = net::RelevantFor(w, 1, glm::vec3{0.f}, settings);
	CHECK(Contains(relevant, nearEntity));
	CHECK_FALSE(Contains(relevant, farEntity));
}

TEST_CASE("A connection always receives the entity it owns, however distant")
{
	World w;
	const Entity mine = MakeNetworked(w, {9999.f, 0.f, 0.f}, 7);

	net::RelevancySettings settings;
	settings.radius = 10.f;

	const std::vector<Entity> relevant = net::RelevantFor(w, 7, glm::vec3{0.f}, settings);
	CHECK(Contains(relevant, mine));
}

TEST_CASE("Disabling relevancy returns every networked entity")
{
	World w;
	const Entity a = MakeNetworked(w, {0.f, 0.f, 0.f}, 0);
	const Entity b = MakeNetworked(w, {10000.f, 0.f, 0.f}, 0);

	net::RelevancySettings settings;
	settings.enabled = false;
	settings.radius = 1.f;

	const std::vector<Entity> relevant = net::RelevantFor(w, 1, glm::vec3{0.f}, settings);
	CHECK(Contains(relevant, a));
	CHECK(Contains(relevant, b));
}
