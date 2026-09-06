// Regression coverage for the character-controller authority gap: CharacterExports.cpp
// had no ownership check at all - a script could call Character.Move (or SetVelocity,
// or Jump) on ANY entity, not only ones the caller owns. CharacterControllerComponent::
// locallySimulated already stops a non-owner's StepCharacters from integrating the
// result, but that alone leaves the script-input fields themselves writable by anyone;
// this is the defense-in-depth guard at the export boundary itself, so the engine-level
// contract holds regardless of what any particular prefab happens to attach. These tests
// call the exported C-ABI functions directly (extern "C", so no CoreCLR host is needed)
// against a real World + NetworkContext, in the same not-yet-owned shape
// NetworkContextTests.cpp already establishes for IsOwner/HasAuthority.
#include <doctest/doctest.h>

#include "net/NetComponents.hpp"
#include "net/NetworkContext.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/interop/InteropCommon.hpp"
#include "utils/ServiceContainer.hpp"

using aether::app::scripting::interop::Vec3;

extern "C" void aether_character_add(std::uint32_t id, float radius, float halfHeight);
extern "C" void aether_character_move(std::uint32_t id, Vec3 direction, float speed);
extern "C" void aether_character_set_velocity(std::uint32_t id, Vec3 velocity);
extern "C" void aether_character_jump(std::uint32_t id, float speed);

namespace
{
	constexpr std::uint16_t kUnreachablePort = 24703; // nothing listens here, deliberately

	struct CharacterExportsFixture
	{
		aether::World world;
		aether::ServiceContainer services;
		aether::app::scripting::SceneContext ctx;
		aether::app::scripting::ActiveContextScope scope;
		aether::net::NetworkContext* net = nullptr;

		CharacterExportsFixture()
		      : scope(ctx)
		{
			world.SetSceneKind(aether::SceneKind::Scene3D);
			world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
			auto context = std::make_unique<aether::net::NetworkContext>(services);
			net = context.get();
			services.RegisterOwned<aether::net::NetworkContext>(std::move(context));
			ctx.world = &world;
			ctx.services = &services;
		}

		aether::Entity MakeEntity()
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e);
			return e;
		}
	};
} // namespace

TEST_CASE("Character.Move/SetVelocity/Jump are all refused on an entity this peer does not own")
{
	CharacterExportsFixture fx;
	REQUIRE(fx.net->StartClient(fx.world, "127.0.0.1", kUnreachablePort));
	fx.net->Session().SetLocalConnection(2);

	const aether::Entity notOwned = fx.MakeEntity();
	fx.world.Emplace<aether::net::NetworkIdentity>(notOwned).owner = 7; // somebody else
	aether_character_add(notOwned.id, 0.4f, 0.9f);
	REQUIRE_FALSE(fx.net->HasAuthority(fx.world, notOwned));

	aether_character_move(notOwned.id, Vec3{1.0f, 0.0f, 0.0f}, 5.0f);
	aether_character_set_velocity(notOwned.id, Vec3{0.0f, 3.0f, 0.0f});
	aether_character_jump(notOwned.id, 8.0f);

	const auto* cc = fx.world.TryGet<aether::CharacterControllerComponent>(notOwned);
	REQUIRE(cc != nullptr);
	CHECK(cc->desiredVelocity == glm::vec3{0.0f});
	CHECK(cc->pendingJumpSpeed == doctest::Approx(0.0f));

	fx.net->Stop(fx.world);
}

TEST_CASE("Character.Move/SetVelocity/Jump all still work on an entity this peer owns")
{
	// The guard must not become a blanket refusal - an owned character (or any
	// character in an offline game, where HasAuthority is unconditionally true) must
	// keep working exactly as before.
	CharacterExportsFixture fx;
	REQUIRE(fx.net->StartClient(fx.world, "127.0.0.1", kUnreachablePort));
	fx.net->Session().SetLocalConnection(2);

	const aether::Entity owned = fx.MakeEntity();
	fx.world.Emplace<aether::net::NetworkIdentity>(owned).owner = 2; // this peer
	aether_character_add(owned.id, 0.4f, 0.9f);
	REQUIRE(fx.net->HasAuthority(fx.world, owned));

	aether_character_move(owned.id, Vec3{1.0f, 0.0f, 0.0f}, 5.0f);
	CHECK(fx.world.Get<aether::CharacterControllerComponent>(owned).desiredVelocity.x == doctest::Approx(5.0f));

	aether_character_set_velocity(owned.id, Vec3{0.0f, 3.0f, 0.0f});
	CHECK(fx.world.Get<aether::CharacterControllerComponent>(owned).desiredVelocity.y == doctest::Approx(3.0f));

	aether_character_jump(owned.id, 8.0f);
	CHECK(fx.world.Get<aether::CharacterControllerComponent>(owned).pendingJumpSpeed == doctest::Approx(8.0f));

	fx.net->Stop(fx.world);
}

TEST_CASE("Character.Move works with no NetworkContext registered at all (offline build)")
{
	// A single-player build never registers a NetworkContext (SceneContext::services can
	// be null too) - the guard must read that as "allowed", not silently disable every
	// character in the game.
	aether::World world;
	world.SetSceneKind(aether::SceneKind::Scene3D);
	world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
	aether::app::scripting::SceneContext ctx;
	aether::app::scripting::ActiveContextScope scope(ctx);
	ctx.world = &world;
	ctx.services = nullptr;

	const aether::Entity e = world.Create();
	world.Emplace<aether::TransformComponent>(e);
	aether_character_add(e.id, 0.4f, 0.9f);

	aether_character_move(e.id, Vec3{1.0f, 0.0f, 0.0f}, 5.0f);
	CHECK(world.Get<aether::CharacterControllerComponent>(e).desiredVelocity.x == doctest::Approx(5.0f));
}
