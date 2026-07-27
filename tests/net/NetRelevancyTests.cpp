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

	// A networked entity with no TransformComponent - a game-state entity that
	// replicates only script fields and has no position to be far from.
	Entity MakeTransformless(World& w, net::ConnectionId owner)
	{
		const Entity e = w.Create();
		auto& id = w.Emplace<net::NetworkIdentity>(e);
		id.netId = static_cast<std::uint32_t>(e.id);
		id.owner = owner;
		return e;
	}

	bool Contains(const std::vector<Entity>& v, Entity e)
	{
		return std::find(v.begin(), v.end(), e) != v.end();
	}

	std::size_t CountOf(const std::vector<Entity>& v, Entity e)
	{
		return static_cast<std::size_t>(std::count(v.begin(), v.end(), e));
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

TEST_CASE("A transformless networked entity is invisible to the spatial query")
{
	// Pins the trap RelevantWithTransformless exists to close: the spatial view
	// cannot see an entity with no position, so it would replicate to nobody.
	World w;
	const Entity gameState = MakeTransformless(w, 0);

	net::RelevancySettings settings;
	CHECK_FALSE(Contains(net::RelevantFor(w, 1, glm::vec3{0.f}, settings), gameState));
}

TEST_CASE("A transformless networked entity is always relevant to the send set")
{
	World w;
	const Entity gameState = MakeTransformless(w, 0);
	const Entity nearEntity = MakeNetworked(w, {1.f, 0.f, 0.f}, 0);
	const Entity farEntity = MakeNetworked(w, {5000.f, 0.f, 0.f}, 0);

	net::RelevancySettings settings;
	settings.radius = 10.f;

	const std::vector<Entity> relevant = net::RelevantWithTransformless(w, 1, glm::vec3{0.f}, settings);
	CHECK(Contains(relevant, gameState));
	CHECK(Contains(relevant, nearEntity));
	CHECK_FALSE(Contains(relevant, farEntity));

	// Unioned, not appended blindly: a spatial entity must not be sent twice.
	CHECK(CountOf(relevant, gameState) == 1);
	CHECK(CountOf(relevant, nearEntity) == 1);
}

TEST_CASE("Transformless entities bypass relevancy even at zero radius")
{
	World w;
	const Entity gameState = MakeTransformless(w, 4);

	net::RelevancySettings settings;
	settings.radius = 0.f;

	// Viewed by a connection that does not own it, from anywhere, with no radius:
	// there is no distance at which shared game state stops mattering.
	CHECK(Contains(net::RelevantWithTransformless(w, 9, glm::vec3{1000.f}, settings), gameState));
}
