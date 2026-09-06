// Velocity replication: NetMessage::VelocitySnapshot carries each qualifying
// entity's AUTHORITATIVE linear/angular velocity - read from a live Jolt body
// when this peer owns it, relayed from NetReceivedVelocity when it does not (a
// host forwarding another connection's entity) - so InterpolationBuffer can
// extrapolate a thrown prop from what its owner actually knows instead of only a
// finite-difference guess from position history.

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetVelocity.hpp"
#include "net/NetworkContext.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;
using namespace aether::net;

namespace
{
	constexpr std::uint16_t kHostPort = 24991; // distinct from every other net test's ports

	Entity MakeDynamicBody(World& world, PhysicsSystem& physics, glm::vec3 pos)
	{
		const Entity e = world.Create();
		world.Emplace<TransformComponent>(e,
		        TransformComponent{.localToWorld = ComposeTransform(pos, {0.f, 0.f, 0.f}, {1.f, 1.f, 1.f})});
		world.Emplace<RigidBodyComponent>(e, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
		world.Emplace<ColliderComponent>(e,
		        ColliderComponent{.shape = PhysicsShapeType::Box, .halfExtents = {0.5f, 0.5f, 0.5f}});
		physics.Update(world, PhysicsSystem::kFixedTimestep); // bakes the pending body
		return e;
	}
} // namespace

TEST_CASE("EncodeVelocitySnapshot/DecodeVelocitySnapshot round-trips linear and angular velocity")
{
	const std::vector<VelocityEntry> entries{
	        {.netId = 5, .linear = {1.f, 2.f, 3.f}, .angular = {0.f, 4.f, 0.f}},
	        {.netId = 9, .linear = {-1.f, 0.f, 0.f}, .angular = {0.f, 0.f, 0.f}},
	};
	const std::vector<std::byte> bytes = EncodeVelocitySnapshot(entries);

	ByteReader r{bytes};
	const auto decoded = DecodeVelocitySnapshot(r);
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->size() == 2);
	CHECK((*decoded)[0].netId == 5);
	CHECK((*decoded)[0].linear == glm::vec3(1.f, 2.f, 3.f));
	CHECK((*decoded)[0].angular == glm::vec3(0.f, 4.f, 0.f));
	CHECK((*decoded)[1].netId == 9);
}

TEST_CASE("A truncated velocity packet decodes to nothing rather than to garbage")
{
	ByteWriter w;
	w.U16(1);
	w.U32(5);
	w.F32(1.f); // linear.x only; the rest of the entry never arrived
	ByteReader r{w.View()};
	CHECK_FALSE(DecodeVelocitySnapshot(r).has_value());
}

TEST_CASE("BuildVelocitySnapshot reports the live physics velocity of an owned Dynamic body")
{
	ServiceContainer services;
	World world;
	world.SetSceneKind(SceneKind::Scene3D);
	world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
	auto system = std::make_unique<PhysicsSystem>();
	PhysicsSystem& physics = *system;
	world.RegisterSystem(std::move(system));
	NetworkContext context(services); // Offline: OwnsIdentity is unconditionally true

	const Entity body = MakeDynamicBody(world, physics, {0.f, 5.f, 0.f});
	auto& identity = world.Emplace<NetworkIdentity>(body);
	identity.netId = 11; // offline never assigns one, but Build only checks != 0
	REQUIRE(world.Get<RigidBodyComponent>(body).body.IsValid());

	physics.SetLinearVelocity(world.Get<RigidBodyComponent>(body).body, {3.f, 0.f, 0.f});
	physics.SetAngularVelocity(world.Get<RigidBodyComponent>(body).body, {0.f, 1.5f, 0.f});

	const std::vector<std::byte> bytes = BuildVelocitySnapshot(world, context, {body});
	REQUIRE_FALSE(bytes.empty());

	ByteReader r{bytes};
	const auto decoded = DecodeVelocitySnapshot(r);
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->size() == 1);
	CHECK((*decoded)[0].netId == 11);
	CHECK((*decoded)[0].linear.x == doctest::Approx(3.f).epsilon(0.01));
	CHECK((*decoded)[0].angular.y == doctest::Approx(1.5f).epsilon(0.01));
}

TEST_CASE("BuildVelocitySnapshot reports nothing for a Kinematic body or one with no RigidBodyComponent")
{
	ServiceContainer services;
	NetworkContext context(services);
	World world;

	const Entity kinematic = world.Create();
	world.Emplace<NetworkIdentity>(kinematic, NetworkIdentity{.netId = 1});
	world.Emplace<RigidBodyComponent>(kinematic, RigidBodyComponent{.motionType = PhysicsMotionType::Kinematic});

	const Entity bodyless = world.Create();
	world.Emplace<NetworkIdentity>(bodyless, NetworkIdentity{.netId = 2});

	CHECK(BuildVelocitySnapshot(world, context, {kinematic, bodyless}).empty());
}

TEST_CASE("BuildVelocitySnapshot relays NetReceivedVelocity for an entity this peer does not own")
{
	ServiceContainer services;
	World world;
	NetworkContext context(services);
	REQUIRE(context.StartHost(world, kHostPort, 4));

	const Entity clientOwned = world.Create();
	world.Emplace<NetworkIdentity>(clientOwned, NetworkIdentity{.netId = 6, .owner = 5}); // not this host
	world.Emplace<NetReceivedVelocity>(clientOwned, NetReceivedVelocity{.linear = {2.f, 0.f, 0.f}, .angular = {}});

	const std::vector<std::byte> bytes = BuildVelocitySnapshot(world, context, {clientOwned});
	REQUIRE_FALSE(bytes.empty());
	ByteReader r{bytes};
	const auto decoded = DecodeVelocitySnapshot(r);
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->size() == 1);
	CHECK((*decoded)[0].linear == glm::vec3(2.f, 0.f, 0.f));

	context.Stop(world);
}

TEST_CASE("BuildVelocitySnapshot reports nothing for a not-owned entity with no NetReceivedVelocity yet")
{
	ServiceContainer services;
	World world;
	NetworkContext context(services);
	REQUIRE(context.StartHost(world, kHostPort + 1, 4));

	const Entity clientOwned = world.Create();
	world.Emplace<NetworkIdentity>(clientOwned, NetworkIdentity{.netId = 6, .owner = 5});

	CHECK(BuildVelocitySnapshot(world, context, {clientOwned}).empty());

	context.Stop(world);
}

TEST_CASE("ApplyVelocitySnapshot writes NetReceivedVelocity and reports what it accepted")
{
	World world;
	NetSession session;
	session.SetRole(NetRole::Client);
	const Entity entity = world.Create();
	session.Bind(4, entity);
	world.Emplace<NetworkIdentity>(entity, NetworkIdentity{.netId = 4});

	const std::vector<VelocityEntry> entries{{.netId = 4, .linear = {1.f, 2.f, 3.f}, .angular = {0.f, 0.f, 1.f}}};
	const std::vector<std::byte> bytes = EncodeVelocitySnapshot(entries);

	const std::vector<VelocityEntry> applied = ApplyVelocitySnapshot(world, session, bytes, StateWriteGate::TrustAll());

	REQUIRE(applied.size() == 1);
	CHECK(applied[0].netId == 4);
	const auto* received = world.TryGet<NetReceivedVelocity>(entity);
	REQUIRE(received != nullptr);
	CHECK(received->linear == glm::vec3(1.f, 2.f, 3.f));
	CHECK(received->angular == glm::vec3(0.f, 0.f, 1.f));
}

TEST_CASE("ApplyVelocitySnapshot refuses an entry the write gate does not allow")
{
	World world;
	NetSession session;
	session.SetRole(NetRole::Host);
	const Entity entity = world.Create();
	session.Bind(4, entity);
	world.Emplace<NetworkIdentity>(entity, NetworkIdentity{.netId = 4, .owner = 5});

	const std::vector<VelocityEntry> entries{{.netId = 4, .linear = {1.f, 0.f, 0.f}}};
	const std::vector<std::byte> bytes = EncodeVelocitySnapshot(entries);

	// Sender claims to be connection 9; the entity is owned by connection 5.
	const std::vector<VelocityEntry> applied = ApplyVelocitySnapshot(world, session, bytes, StateWriteGate::OwnedBy(9));

	CHECK(applied.empty());
	CHECK(world.TryGet<NetReceivedVelocity>(entity) == nullptr);
}
