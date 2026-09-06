// Regression coverage for the "Physics.SetLinearVelocity silently dropped on a fresh
// body" bug: PhysicsExports.cpp's velocity/impulse exports looked up a RigidBodyComponent
// but called into PhysicsSystem with its (still-invalid) PhysicsBodyHandle when the Jolt
// body had not been baked yet by FlushPendingBodies, which is a same-frame no-op that
// PhysicsSystem::SetLinearVelocity swallows silently. These tests call the exported C-ABI
// functions directly (extern "C", so no CoreCLR host is needed) against a real
// World+PhysicsSystem, exactly the "add a body, then set its velocity" sequence the bug
// report used.
#include <doctest/doctest.h>

#include "net/NetComponents.hpp"
#include "net/NetworkContext.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/interop/InteropCommon.hpp"
#include "utils/ServiceContainer.hpp"

using aether::app::scripting::interop::Vec3;

extern "C" void aether_physics_add_box(std::uint32_t id, Vec3 halfExtents, std::int32_t dynamic);
extern "C" void aether_physics_set_linear_velocity(std::uint32_t id, Vec3 velocity);
extern "C" Vec3 aether_physics_get_linear_velocity(std::uint32_t id);
extern "C" void aether_physics_set_angular_velocity(std::uint32_t id, Vec3 velocity);
extern "C" Vec3 aether_physics_get_angular_velocity(std::uint32_t id);
extern "C" void aether_physics_add_impulse(std::uint32_t id, Vec3 impulse);
extern "C" void aether_physics_add_impulse_at_point(std::uint32_t id, Vec3 impulse, Vec3 point);
extern "C" void aether_physics_set_gravity_factor(std::uint32_t id, float factor);
extern "C" void aether_physics_set_body_active(std::uint32_t id, std::int32_t active);
extern "C" std::int32_t aether_physics_is_body_active(std::uint32_t id);

namespace
{
	struct ExportsFixture
	{
		aether::World world;
		aether::PhysicsSystem* physics = nullptr;
		aether::app::scripting::SceneContext ctx;
		aether::app::scripting::ActiveContextScope scope;

		ExportsFixture()
		      : scope(ctx)
		{
			world.SetSceneKind(aether::SceneKind::Scene3D);
			world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
			auto system = std::make_unique<aether::PhysicsSystem>();
			physics = system.get();
			world.RegisterSystem(std::move(system));
			ctx.world = &world;
			ctx.physics = physics;
		}

		aether::Entity MakeFallingProp(glm::vec3 pos)
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			return e;
		}

		void StepSeconds(float seconds)
		{
			const int frames = static_cast<int>(seconds / aether::PhysicsSystem::kFixedTimestep + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				physics->Update(world, aether::PhysicsSystem::kFixedTimestep);
			}
		}

		[[nodiscard]] glm::vec3 PositionOf(aether::Entity e)
		{
			return world.Get<aether::PhysicsStateComponent>(e).currPosition;
		}
	};
} // namespace

TEST_CASE("Physics.SetLinearVelocity right after AddBoxBody is not silently dropped")
{
	ExportsFixture fx;
	const aether::Entity prop = fx.MakeFallingProp({0.0f, 5.0f, 0.0f});

	// Exactly the sequence the bug report describes: add a body, then immediately set its
	// velocity, both before the body has ever been baked by FlushPendingBodies.
	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	aether_physics_set_linear_velocity(prop.id, Vec3{5.0f, 0.0f, 0.0f});

	// Read-your-own-write before the body exists: must reflect the seeded velocity
	// (RigidBodyComponent::initialVelocity), not silently read back zero.
	const Vec3 preBake = aether_physics_get_linear_velocity(prop.id);
	CHECK(preBake.x == doctest::Approx(5.0f));

	fx.StepSeconds(1.0f);

	// Fell under gravity AND was launched sideways. Before the fix this stayed near
	// x = 0: the velocity call landed on an invalid handle and PhysicsSystem::
	// SetLinearVelocity silently no-oped.
	CHECK(fx.PositionOf(prop).x > 2.0f);
}

TEST_CASE("Physics.SetAngularVelocity right after AddBoxBody is not silently dropped")
{
	ExportsFixture fx;
	const aether::Entity prop = fx.MakeFallingProp({0.0f, 5.0f, 0.0f});

	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	aether_physics_set_angular_velocity(prop.id, Vec3{0.0f, 8.0f, 0.0f});

	const Vec3 preBake = aether_physics_get_angular_velocity(prop.id);
	CHECK(preBake.y == doctest::Approx(8.0f));

	// One substep is enough to prove the spin was actually applied to the live body
	// (GetAngularVelocity now reads through PhysicsSystem once the handle is valid).
	fx.StepSeconds(aether::PhysicsSystem::kFixedTimestep);
	const Vec3 postBake = aether_physics_get_angular_velocity(prop.id);
	CHECK(postBake.y > 1.0f);
}

TEST_CASE("Physics.AddImpulse before the body exists is honestly lost, not silently faked")
{
	ExportsFixture fx;
	const aether::Entity prop = fx.MakeFallingProp({0.0f, 5.0f, 0.0f});

	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	// Lands before the body exists. An impulse depends on mass, which is not resolved
	// until body creation, so there is nothing correct to seed - unlike velocity, this
	// one is meant to be discarded (with a warning), not applied.
	aether_physics_add_impulse(prop.id, Vec3{50.0f, 0.0f, 0.0f});

	fx.StepSeconds(0.5f);

	// No sideways launch: the deferred impulse must not have been silently approximated
	// from a guessed mass.
	CHECK(fx.PositionOf(prop).x == doctest::Approx(0.0f).epsilon(0.05));

	// The same call against the NOW-valid body still works normally. A 1 m cube box
	// defaults to a ~1000 kg density-derived mass, so this needs a proportionally larger
	// impulse than the pre-bake one above to move it measurably in half a second.
	aether_physics_add_impulse(prop.id, Vec3{2000.0f, 0.0f, 0.0f});
	fx.StepSeconds(0.5f);
	CHECK(fx.PositionOf(prop).x > 0.5f);
}

TEST_CASE("Physics.AddImpulseAtPoint imparts spin, unlike a centre-of-mass impulse")
{
	ExportsFixture fx;
	const aether::Entity prop = fx.MakeFallingProp({0.0f, 5.0f, 0.0f});
	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	fx.StepSeconds(aether::PhysicsSystem::kFixedTimestep); // bake the body first

	REQUIRE(aether_physics_get_angular_velocity(prop.id).z == doctest::Approx(0.0f));

	// Struck at the top face, not the centre of mass: an X impulse there is exactly a
	// torque around Z (offset (0, 0.5, 0) cross impulse (X, 0, 0) = (0, 0, -0.5 * X)) -
	// what makes a thrown prop tumble instead of sliding flat.
	aether_physics_add_impulse_at_point(prop.id, Vec3{2000.0f, 0.0f, 0.0f}, Vec3{0.0f, 5.5f, 0.0f});

	CHECK(aether_physics_get_angular_velocity(prop.id).z < -0.1f);
}

TEST_CASE("Physics.SetGravityFactor(0) right after AddBoxBody keeps the body from falling")
{
	ExportsFixture fx;
	const aether::Entity prop = fx.MakeFallingProp({0.0f, 5.0f, 0.0f});

	// Deferred exactly like the velocity setters: lands before the body exists, seeds
	// RigidBodyComponent::gravityFactor, which FlushPendingBodies already applies at
	// creation - a grab controller zeroing a just-picked-up prop's gravity in the same
	// call that adds its body must not lose the write to a one-frame race.
	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	aether_physics_set_gravity_factor(prop.id, 0.0f);

	fx.StepSeconds(1.0f);

	CHECK(fx.PositionOf(prop).y == doctest::Approx(5.0f).epsilon(0.02));
}

TEST_CASE("Physics.SetBodyActive(false) right after AddBoxBody creates the body asleep")
{
	ExportsFixture fx;
	const aether::Entity prop = fx.MakeFallingProp({0.0f, 5.0f, 0.0f});

	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	// Deferred: seeds RigidBodyComponent::startActive, which decides bi.AddBody's
	// activation mode at creation - a grab controller must be able to pick up a prop and
	// immediately freeze it in one call, not wait a frame for the body to exist first.
	aether_physics_set_body_active(prop.id, 0);

	fx.StepSeconds(0.5f);
	CHECK_FALSE(aether_physics_is_body_active(prop.id));
	// Inactive means not simulating at all: it must not have fallen one bit.
	CHECK(fx.PositionOf(prop).y == doctest::Approx(5.0f).epsilon(0.001));

	// Waking the now-live body is the other half of the same grab-controller need.
	aether_physics_set_body_active(prop.id, 1);
	fx.StepSeconds(0.5f);
	CHECK(aether_physics_is_body_active(prop.id));
	CHECK(fx.PositionOf(prop).y < 4.9f);
}

namespace
{
	constexpr std::uint16_t kUnreachablePort = 24704; // nothing listens here, deliberately

	// A second fixture, distinct from ExportsFixture above: these two tests need a real
	// NetworkContext (registered through ServiceContainer, exactly like
	// CharacterExportsTests.cpp's CharacterExportsFixture) to exercise the ownership
	// gate CanControl adds - ExportsFixture's plain SceneContext (no services at all)
	// already covers every existing test's implicit "offline" case above.
	struct NetworkedExportsFixture
	{
		aether::World world;
		aether::PhysicsSystem* physics = nullptr;
		aether::ServiceContainer services;
		aether::app::scripting::SceneContext ctx;
		aether::app::scripting::ActiveContextScope scope;
		aether::net::NetworkContext* net = nullptr;

		NetworkedExportsFixture()
		      : scope(ctx)
		{
			world.SetSceneKind(aether::SceneKind::Scene3D);
			world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
			auto system = std::make_unique<aether::PhysicsSystem>();
			physics = system.get();
			world.RegisterSystem(std::move(system));
			auto context = std::make_unique<aether::net::NetworkContext>(services);
			net = context.get();
			services.RegisterOwned<aether::net::NetworkContext>(std::move(context));
			ctx.world = &world;
			ctx.physics = physics;
			ctx.services = &services;
		}
	};
} // namespace

TEST_CASE("Physics.SetLinearVelocity/AddImpulse/SetGravityFactor are refused on a body this peer does not own")
{
	NetworkedExportsFixture fx;
	REQUIRE(fx.net->StartClient(fx.world, "127.0.0.1", kUnreachablePort));
	fx.net->Session().SetLocalConnection(2);

	const aether::Entity prop = fx.world.Create();
	fx.world.Emplace<aether::TransformComponent>(prop, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	fx.world.Emplace<aether::net::NetworkIdentity>(prop).owner = 7; // somebody else
	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	fx.physics->Update(fx.world, aether::PhysicsSystem::kFixedTimestep); // bake the body
	REQUIRE_FALSE(fx.net->HasAuthority(fx.world, prop));

	aether_physics_set_linear_velocity(prop.id, Vec3{5.0f, 0.0f, 0.0f});
	aether_physics_add_impulse(prop.id, Vec3{2000.0f, 0.0f, 0.0f});
	aether_physics_set_gravity_factor(prop.id, 0.0f);

	CHECK(aether_physics_get_linear_velocity(prop.id).x == doctest::Approx(0.0f));
	for (int i = 0; i < 30; ++i)
	{
		fx.physics->Update(fx.world, aether::PhysicsSystem::kFixedTimestep);
	}
	// Gravity factor refused means it kept falling instead of floating in place.
	CHECK(fx.world.Get<aether::PhysicsStateComponent>(prop).currPosition.y < 4.9f);

	fx.net->Stop(fx.world);
}

TEST_CASE("Physics.SetLinearVelocity/AddImpulse/SetGravityFactor still work on a body this peer owns")
{
	NetworkedExportsFixture fx;
	REQUIRE(fx.net->StartClient(fx.world, "127.0.0.1", kUnreachablePort));
	fx.net->Session().SetLocalConnection(2);

	const aether::Entity prop = fx.world.Create();
	fx.world.Emplace<aether::TransformComponent>(prop, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	fx.world.Emplace<aether::net::NetworkIdentity>(prop).owner = 2; // this peer
	aether_physics_add_box(prop.id, Vec3{0.5f, 0.5f, 0.5f}, 1);
	fx.physics->Update(fx.world, aether::PhysicsSystem::kFixedTimestep); // bake the body
	REQUIRE(fx.net->HasAuthority(fx.world, prop));

	aether_physics_set_linear_velocity(prop.id, Vec3{5.0f, 0.0f, 0.0f});
	CHECK(aether_physics_get_linear_velocity(prop.id).x == doctest::Approx(5.0f));

	fx.net->Stop(fx.world);
}
