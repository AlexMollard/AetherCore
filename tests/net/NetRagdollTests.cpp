// Ragdoll ownership grouping and bone-pose replication: ownership must resolve a
// grabbed LIMB to the ragdoll's ROOT (the only entity with a NetworkIdentity at
// all - see RagdollBoneComponent's own comment), and BuildRagdollPoseSnapshot/
// ApplyRagdollPoses must keep every OTHER bone's index-addressed pose aligned
// between the two ends without ever writing pose data onto the wrong limb.

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "net/NetComponents.hpp"
#include "net/NetRagdoll.hpp"
#include "net/NetSerialize.hpp"
#include "net/NetSession.hpp"
#include "net/NetSnapshot.hpp"
#include "net/NetworkContext.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;
using namespace aether::net;

namespace
{
	constexpr std::uint16_t kHostPort = 24990; // distinct from every other net test's ports

	Entity MakeBone(World& world, Entity root, glm::vec3 pos, glm::vec3 eulerDeg)
	{
		const Entity bone = world.Create();
		world.Emplace<TransformComponent>(bone,
		        TransformComponent{.localToWorld = ComposeTransform(pos, eulerDeg, {1.f, 1.f, 1.f})});
		if (root.IsValid())
		{
			world.Emplace<RagdollBoneComponent>(bone, RagdollBoneComponent{.root = root});
		}
		return bone;
	}
} // namespace

TEST_CASE("EncodeRagdollPoses/DecodeRagdollPoses round-trips every bone of every entry")
{
	const std::vector<net::RagdollPoseEntry> entries{
	        {.netId = 7, .bones = {{{1.f, 2.f, 3.f}, {4.f, 5.f, 6.f}}, {{7.f, 8.f, 9.f}, {10.f, 11.f, 12.f}}}},
	        {.netId = 9, .bones = {{{-1.f, 0.f, 0.f}, {0.f, 90.f, 0.f}}}},
	};

	const std::vector<std::byte> bytes = net::EncodeRagdollPoses(entries);
	net::ByteReader r{bytes};
	const auto decoded = net::DecodeRagdollPoses(r);
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->size() == 2);
	CHECK((*decoded)[0].netId == 7);
	REQUIRE((*decoded)[0].bones.size() == 2);
	CHECK((*decoded)[0].bones[1].position == glm::vec3(7.f, 8.f, 9.f));
	CHECK((*decoded)[0].bones[1].rotation == glm::vec3(10.f, 11.f, 12.f));
	CHECK((*decoded)[1].netId == 9);
	REQUIRE((*decoded)[1].bones.size() == 1);
	CHECK((*decoded)[1].bones[0].rotation == glm::vec3(0.f, 90.f, 0.f));
}

TEST_CASE("A truncated ragdoll pose packet decodes to nothing rather than to garbage")
{
	net::ByteWriter w;
	w.U16(1);
	w.U32(7);
	w.U8(2); // claims two bones
	w.F32(1.f); // but only one float follows
	net::ByteReader r{w.View()};
	CHECK_FALSE(net::DecodeRagdollPoses(r).has_value());
}

TEST_CASE("BuildRagdollPoseSnapshot reports every non-root bone, in RagdollComponent::bones order")
{
	World world;
	const Entity root = world.Create();
	world.Emplace<NetworkIdentity>(root, NetworkIdentity{.netId = 42});
	world.Emplace<TransformComponent>(root);
	const Entity limbA = MakeBone(world, root, {1.f, 0.f, 0.f}, {0.f, 0.f, 0.f});
	const Entity limbB = MakeBone(world, root, {2.f, 0.f, 0.f}, {0.f, 45.f, 0.f});
	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, limbA, limbB}});

	const std::vector<std::byte> bytes = net::BuildRagdollPoseSnapshot(world, {root});
	REQUIRE_FALSE(bytes.empty());

	net::ByteReader r{bytes};
	const auto decoded = net::DecodeRagdollPoses(r);
	REQUIRE(decoded.has_value());
	REQUIRE(decoded->size() == 1);
	CHECK((*decoded)[0].netId == 42);
	REQUIRE((*decoded)[0].bones.size() == 2); // root itself excluded
	CHECK((*decoded)[0].bones[0].position == glm::vec3(1.f, 0.f, 0.f));
	CHECK((*decoded)[0].bones[1].position == glm::vec3(2.f, 0.f, 0.f));
	CHECK((*decoded)[0].bones[1].rotation.y == doctest::Approx(45.f));
}

TEST_CASE("BuildRagdollPoseSnapshot skips a candidate with no RagdollComponent or no net id")
{
	World world;
	const Entity plain = world.Create();
	world.Emplace<NetworkIdentity>(plain, NetworkIdentity{.netId = 1});
	world.Emplace<TransformComponent>(plain);

	const Entity unbound = world.Create();
	world.Emplace<NetworkIdentity>(unbound, NetworkIdentity{.netId = 0});
	world.Emplace<RagdollComponent>(unbound, RagdollComponent{.bones = {unbound}});

	CHECK(net::BuildRagdollPoseSnapshot(world, {plain, unbound}).empty());
}

TEST_CASE("BuildRagdollPoseSnapshot drops the whole ragdoll rather than shift bone indices when one bone has no transform")
{
	// The correctness property under test: a silently-skipped mid-list bone would
	// shift every LATER bone's index, and the receiver would apply bone N's pose
	// to bone N-1 - a wrong-limb bug worse than sending nothing for this tick.
	World world;
	const Entity root = world.Create();
	world.Emplace<NetworkIdentity>(root, NetworkIdentity{.netId = 5});
	world.Emplace<TransformComponent>(root);
	const Entity limbA = MakeBone(world, root, {1.f, 0.f, 0.f}, {});
	const Entity limbBNoTransform = world.Create(); // deliberately no TransformComponent
	world.Emplace<RagdollBoneComponent>(limbBNoTransform, RagdollBoneComponent{.root = root});
	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, limbA, limbBNoTransform}});

	CHECK(net::BuildRagdollPoseSnapshot(world, {root}).empty());
}

TEST_CASE("ApplyRagdollPoses writes every non-root bone's transform, preserving its scale")
{
	World world;
	NetSession session;
	session.SetRole(NetRole::Client);

	const Entity root = world.Create();
	world.Emplace<NetworkIdentity>(root, NetworkIdentity{.netId = 3, .owner = net::kInvalidConnection});
	session.Bind(3, root);
	const Entity limbA = MakeBone(world, root, {0.f, 0.f, 0.f}, {});
	world.TryGet<TransformComponent>(limbA)->localToWorld
	        = ComposeTransform({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, {2.f, 2.f, 2.f}); // a distinctive scale to preserve
	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, limbA}});

	const std::vector<net::RagdollPoseEntry> entries{{.netId = 3, .bones = {{{9.f, 8.f, 7.f}, {0.f, 30.f, 0.f}}}}};
	const std::vector<std::byte> bytes = net::EncodeRagdollPoses(entries);

	net::ApplyRagdollPoses(world, session, bytes, net::StateWriteGate::TrustAll());

	glm::vec3 pos{}, euler{}, scale{};
	DecomposeTRS(world.TryGet<TransformComponent>(limbA)->localToWorld, pos, euler, scale);
	CHECK(pos == glm::vec3(9.f, 8.f, 7.f));
	CHECK(euler.y == doctest::Approx(30.f));
	CHECK(scale == glm::vec3(2.f, 2.f, 2.f));
}

TEST_CASE("ApplyRagdollPoses refuses an entry the write gate does not allow")
{
	World world;
	NetSession session;
	session.SetRole(NetRole::Host);

	const Entity root = world.Create();
	world.Emplace<NetworkIdentity>(root, NetworkIdentity{.netId = 3, .owner = 5});
	session.Bind(3, root);
	const Entity limbA = MakeBone(world, root, {0.f, 0.f, 0.f}, {});
	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, limbA}});

	const std::vector<net::RagdollPoseEntry> entries{{.netId = 3, .bones = {{{9.f, 8.f, 7.f}, {0.f, 0.f, 0.f}}}}};
	const std::vector<std::byte> bytes = net::EncodeRagdollPoses(entries);

	// Sender claims to be connection 9, but the root is owned by connection 5.
	net::ApplyRagdollPoses(world, session, bytes, net::StateWriteGate::OwnedBy(9));

	CHECK(world.TryGet<TransformComponent>(limbA)->localToWorld == glm::mat4(1.f));
}

TEST_CASE("ApplyRagdollPoses tolerates a bone count mismatch rather than writing out of bounds")
{
	World world;
	NetSession session;
	session.SetRole(NetRole::Client);

	const Entity root = world.Create();
	world.Emplace<NetworkIdentity>(root, NetworkIdentity{.netId = 3});
	session.Bind(3, root);
	const Entity onlyLimb = MakeBone(world, root, {0.f, 0.f, 0.f}, {});
	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, onlyLimb}}); // this peer knows one bone

	// The packet names TWO - a version-skewed or hostile peer's claim.
	const std::vector<net::RagdollPoseEntry> entries{
	        {.netId = 3, .bones = {{{1.f, 1.f, 1.f}, {}}, {{2.f, 2.f, 2.f}, {}}}}};
	const std::vector<std::byte> bytes = net::EncodeRagdollPoses(entries);

	net::ApplyRagdollPoses(world, session, bytes, net::StateWriteGate::TrustAll());

	// The one bone this peer actually has still gets the first entry - getting
	// here without an out-of-bounds write or a crash is the rest of the point.
	glm::vec3 pos{}, euler{}, scale{};
	DecomposeTRS(world.TryGet<TransformComponent>(onlyLimb)->localToWorld, pos, euler, scale);
	CHECK(pos == glm::vec3(1.f, 1.f, 1.f));
}

TEST_CASE("Claiming a ragdoll limb redirects ownership to its root")
{
	ServiceContainer services;
	World world;
	net::NetworkContext context(services);
	REQUIRE(context.StartHost(world, kHostPort, 4));

	const Entity root = world.Create();
	auto& identity = world.Emplace<net::NetworkIdentity>(root);
	identity.netId = context.Session().AllocateNetId();
	identity.owner = net::kInvalidConnection;
	context.Session().Bind(identity.netId, root);
	const Entity limb = MakeBone(world, root, {0.f, 0.f, 0.f}, {});
	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, limb}});

	constexpr net::ConnectionId kNewOwner = 7;
	CHECK(context.RequestOwnershipTransfer(world, limb, kNewOwner) == net::OwnershipTransferOutcome::Applied);

	// The root's identity changed - the limb has none of its own to have changed.
	CHECK(world.TryGet<net::NetworkIdentity>(root)->owner == kNewOwner);
	CHECK(context.IsOwner(world, limb) == false); // this peer (the host) is not connection 7
	CHECK(world.TryGet<net::NetworkIdentity>(limb) == nullptr); // confirms the limb never had one to check directly

	context.Stop(world);
}

TEST_CASE("IsOwner on a ragdoll limb answers for the whole ragdoll, not the limb alone")
{
	ServiceContainer services;
	World world;
	net::NetworkContext context(services);
	REQUIRE(context.StartClient(world, "127.0.0.1", kHostPort + 1));
	context.Session().SetLocalConnection(4);

	const Entity root = world.Create();
	auto& identity = world.Emplace<net::NetworkIdentity>(root);
	identity.netId = 12;
	identity.owner = 4; // this client's own
	context.Session().Bind(12, root);
	const Entity limb = MakeBone(world, root, {0.f, 0.f, 0.f}, {});
	world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {root, limb}});

	CHECK(context.IsOwner(world, limb));

	identity.owner = 9; // someone else claimed it
	CHECK_FALSE(context.IsOwner(world, limb));

	context.Stop(world);
}
