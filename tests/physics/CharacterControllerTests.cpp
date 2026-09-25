// Coverage for the Jolt CharacterVirtual-backed CharacterControllerComponent: the parts of
// PhysicsSystem::StepCharacters that are decidable without a GPU or a live editor - slope
// limit, step-up, and ground-state transitions. Each case pins a boundary the controller
// would silently cross if StepCharacters regressed, not a setter echoing a field back.
#include <doctest/doctest.h>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace
{
	struct CharacterFixture
	{
		aether::World world;
		aether::PhysicsSystem* physics = nullptr;

		CharacterFixture()
		{
			world.SetSceneKind(aether::SceneKind::Scene3D);
			world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene3D));
			auto system = std::make_unique<aether::PhysicsSystem>();
			physics = system.get();
			world.RegisterSystem(std::move(system));
		}

		aether::Entity MakeStaticBox(glm::vec3 pos, glm::vec3 halfExtents, glm::vec3 eulerDeg = {0.0f, 0.0f, 0.0f})
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, eulerDeg, {1.0f, 1.0f, 1.0f})});
			world.Emplace<aether::RigidBodyComponent>(e, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Static});
			world.Emplace<aether::ColliderComponent>(e, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Box, .halfExtents = halfExtents});
			return e;
		}

		aether::Entity MakeDynamicBox(glm::vec3 pos, glm::vec3 halfExtents, glm::vec3 initialVelocity, bool continuousCollision = false)
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			world.Emplace<aether::RigidBodyComponent>(e, aether::RigidBodyComponent{.motionType = aether::PhysicsMotionType::Dynamic, .continuousCollision = continuousCollision, .initialVelocity = initialVelocity});
			world.Emplace<aether::ColliderComponent>(e, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Box, .halfExtents = halfExtents});
			return e;
		}

		// feetPos is where the character's base (ground contact point) starts - see
		// CharacterControllerComponent's class comment on the feet-vs-centre convention.
		aether::Entity MakeCharacter(glm::vec3 feetPos, float radius = 0.3f, float halfHeight = 0.6f)
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(feetPos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			world.Emplace<aether::CharacterControllerComponent>(e, aether::CharacterControllerComponent{.radius = radius, .halfHeight = halfHeight});
			return e;
		}

		void StepSeconds(float seconds)
		{
			// Update() accumulates and runs whole fixed steps, so feeding it the timestep
			// exactly is what makes the count predictable.
			const int frames = static_cast<int>(seconds / aether::PhysicsSystem::kFixedTimestep + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				physics->Update(world, aether::PhysicsSystem::kFixedTimestep);
			}
		}

		[[nodiscard]] glm::vec3 FeetPositionOf(aether::Entity e)
		{
			return world.Get<aether::PhysicsStateComponent>(e).currPosition;
		}

		[[nodiscard]] aether::CharacterControllerComponent& ControllerOf(aether::Entity e)
		{
			return world.Get<aether::CharacterControllerComponent>(e);
		}
	};
} // namespace

TEST_CASE("Character controller treats a slope under the max angle as walkable ground")
{
	CharacterFixture fx;
	// Ramp tilted 20 degrees about X - comfortably under the default 45 degree limit.
	fx.MakeStaticBox({0.0f, 0.0f, 0.0f}, {2.5f, 0.1f, 2.5f}, {20.0f, 0.0f, 0.0f});
	const aether::Entity player = fx.MakeCharacter({0.0f, 1.5f, 0.0f});

	fx.StepSeconds(2.0f);

	CHECK(fx.ControllerOf(player).isGrounded);
}

// Grounded, the step used to add the whole gravity vector; its along-slope part survived the
// contact solve, so an idle character crept downhill forever on any walkable slope.
TEST_CASE("A character standing idle on a walkable slope stays where it is")
{
	CharacterFixture fx;
	fx.MakeStaticBox({0.0f, 0.0f, 0.0f}, {5.0f, 0.1f, 5.0f}, {20.0f, 0.0f, 0.0f});
	const aether::Entity player = fx.MakeCharacter({0.0f, 1.5f, 0.0f});
	fx.StepSeconds(1.0f); // land and settle
	REQUIRE(fx.ControllerOf(player).isGrounded);
	const glm::vec3 settled = fx.FeetPositionOf(player);

	fx.StepSeconds(3.0f);

	const glm::vec3 later = fx.FeetPositionOf(player);
	CHECK(glm::length(later - settled) < 0.01f);
	CHECK(fx.ControllerOf(player).isGrounded);
}

TEST_CASE("Character controller treats a slope over the max angle as too steep to stand on")
{
	CharacterFixture fx;
	// Ramp tilted 70 degrees about X - well past the default 45 degree limit; a wall in all
	// but name. Same drop as the walkable case above, so the only variable is the angle.
	fx.MakeStaticBox({0.0f, 0.0f, 0.0f}, {2.5f, 0.1f, 2.5f}, {70.0f, 0.0f, 0.0f});
	const aether::Entity player = fx.MakeCharacter({0.0f, 1.5f, 0.0f});

	fx.StepSeconds(2.0f);

	CHECK_FALSE(fx.ControllerOf(player).isGrounded);
}

TEST_CASE("Character controller steps up a ledge below its step height")
{
	CharacterFixture fx;
	// Lower floor spans x in [-5, 2], top at y = 0.
	fx.MakeStaticBox({-1.5f, -0.5f, 0.0f}, {3.5f, 0.5f, 5.0f});
	// Upper step spans x in [2, 10]; its top sits 0.15 m up - half the default 0.3 m step
	// height, so the controller should climb it without slowing down.
	constexpr float kLedgeHeight = 0.15f;
	fx.MakeStaticBox({6.0f, kLedgeHeight - 0.5f, 0.0f}, {4.0f, 0.5f, 5.0f});

	const aether::Entity player = fx.MakeCharacter({-1.0f, 0.0f, 0.0f});
	fx.StepSeconds(0.5f); // settle onto the lower floor first
	fx.ControllerOf(player).desiredVelocity = {2.0f, 0.0f, 0.0f};
	fx.StepSeconds(4.0f);

	// Climbed onto the upper floor rather than stalling at its face.
	CHECK(fx.FeetPositionOf(player).x > 2.5f);
	CHECK(fx.FeetPositionOf(player).y == doctest::Approx(kLedgeHeight).epsilon(0.3));
}

TEST_CASE("Character controller is blocked by a ledge above its step height")
{
	CharacterFixture fx;
	fx.MakeStaticBox({-1.5f, -0.5f, 0.0f}, {3.5f, 0.5f, 5.0f});
	// Twice the default step height - a crate-sized obstacle, not a stair. Same approach
	// speed and duration as the below-threshold case above.
	constexpr float kLedgeHeight = 0.6f;
	fx.MakeStaticBox({6.0f, kLedgeHeight - 0.5f, 0.0f}, {4.0f, 0.5f, 5.0f});

	const aether::Entity player = fx.MakeCharacter({-1.0f, 0.0f, 0.0f});
	fx.StepSeconds(0.5f);
	fx.ControllerOf(player).desiredVelocity = {2.0f, 0.0f, 0.0f};
	fx.StepSeconds(4.0f);

	// Blocked at the step's vertical face: still on the lower floor, short of the ledge.
	CHECK(fx.FeetPositionOf(player).y < 0.3f);
	CHECK(fx.FeetPositionOf(player).x < 2.5f);
}

TEST_CASE("Character controller ground state transitions from airborne to grounded on landing")
{
	CharacterFixture fx;
	fx.MakeStaticBox({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f}); // floor, top at y = 0
	const aether::Entity player = fx.MakeCharacter({0.0f, 3.0f, 0.0f});

	// One substep in: the body now exists but is still 3 m above the floor.
	fx.StepSeconds(aether::PhysicsSystem::kFixedTimestep);
	CHECK_FALSE(fx.ControllerOf(player).isGrounded);

	fx.StepSeconds(2.0f); // falls the rest of the way and settles

	CHECK(fx.ControllerOf(player).isGrounded);
}

TEST_CASE("Character controller ground state transitions from grounded to airborne on jump")
{
	CharacterFixture fx;
	fx.MakeStaticBox({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f});
	const aether::Entity player = fx.MakeCharacter({0.0f, 1.0f, 0.0f});
	fx.StepSeconds(1.0f);
	REQUIRE(fx.ControllerOf(player).isGrounded);

	fx.ControllerOf(player).pendingJumpSpeed = 6.0f;
	// A few substeps, not one: at 6 m/s a single 1/60 s substep only clears ~0.1 m, which
	// is inside Jolt's own predictive contact distance and would still read as grounded.
	fx.StepSeconds(0.1f);

	CHECK_FALSE(fx.ControllerOf(player).isGrounded);
}

// A respawn writes the transform and raises teleportPending (WorldExports.cpp's
// TeleportBodyToTransform). Without the flush consuming it, the next step writes the
// character's own simulated position straight back over the respawn.
TEST_CASE("A teleport request moves a simulated character to its transform and stops it")
{
	CharacterFixture fx;
	fx.MakeStaticBox({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f});
	const aether::Entity player = fx.MakeCharacter({0.0f, 3.0f, 0.0f});
	fx.StepSeconds(0.5f); // falling at several m/s
	REQUIRE(fx.ControllerOf(player).velocity.y < -2.0f);

	fx.world.Get<aether::TransformComponent>(player).localToWorld = aether::ComposeTransform({20.0f, 10.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
	fx.ControllerOf(player).teleportPending = true;
	// Two updates: controller outputs are synced at the start of the update after a step.
	fx.StepSeconds(2.0f * aether::PhysicsSystem::kFixedTimestep);

	const glm::vec3 feet = fx.FeetPositionOf(player);
	CHECK(feet.x == doctest::Approx(20.0f).epsilon(0.001));
	// Restarted from rest: a couple of substeps of gravity, not the fall speed it had before.
	CHECK(feet.y > 9.95f);
	CHECK(fx.ControllerOf(player).velocity.y > -1.0f);
	CHECK_FALSE(fx.ControllerOf(player).teleportPending);
}

// Surface rules ("the sea drowns you") ask which entity the character stands on. A ray cast
// from the feet cannot answer it: from inside the capsule it hits the character's own inner
// body, from just below it misses a plane the feet rest level with.
TEST_CASE("A character reports the entity it stands on as ground, and none in the air")
{
	CharacterFixture fx;
	const aether::Entity other = fx.MakeStaticBox({20.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f});
	const aether::Entity floor = fx.MakeStaticBox({0.0f, -0.5f, 0.0f}, {5.0f, 0.5f, 5.0f});
	const aether::Entity player = fx.MakeCharacter({0.0f, 0.5f, 0.0f});
	fx.StepSeconds(1.0f);
	REQUIRE(fx.ControllerOf(player).isGrounded);
	CHECK(fx.ControllerOf(player).groundEntity == floor.id);
	CHECK(fx.ControllerOf(player).groundEntity != other.id);

	fx.world.Get<aether::TransformComponent>(player).localToWorld = aether::ComposeTransform({0.0f, 10.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
	fx.ControllerOf(player).teleportPending = true;
	// Updates too short to step: the teleport is flushed, but the outputs are re-synced from the
	// last (pre-teleport) step, so a stale "still on the floor" would show here (a respawn out of
	// the sea drowned twice).
	fx.physics->Update(fx.world, aether::PhysicsSystem::kFixedTimestep * 0.25f);
	fx.physics->Update(fx.world, aether::PhysicsSystem::kFixedTimestep * 0.25f);
	CHECK(fx.ControllerOf(player).groundEntity == 0u);
	CHECK_FALSE(fx.ControllerOf(player).isGrounded);
}

// Proves the Jolt inner rigid body (CharacterVirtualSettings::mInnerBodyShape, see
// FlushPendingCharacters): without it, a character is invisible to Jolt's own rigid-body
// integration - nothing stops a fast dynamic body's own sweep, because there is nothing in
// the broad phase to sweep against.
TEST_CASE("A fast dynamic body launched at a stationary character actually collides")
{
	CharacterFixture fx;
	fx.MakeCharacter({0.0f, 0.0f, 0.0f});
	// Half the capsule's total height (see the feet-vs-centre comment on MakeCharacter) -
	// aimed at the middle of the capsule's cylindrical section, not its rounded cap.
	constexpr float kMidHeight = 0.9f;
	// 30 m/s covers 0.5 m per physics substep - more than the character's own ~0.6 m
	// diameter - so a character with no inner body would have nothing sweep-test against
	// in the one substep it takes to cross. continuousCollision is what actually performs
	// that sweep; Jolt's own CharacterVirtual class comment names this exact case as what
	// the inner body is for ("fast moving objects of motion quality LinearCast will not be
	// able to pass through the CharacterVirtual in 1 time step").
	const aether::Entity crate = fx.MakeDynamicBox({-5.0f, kMidHeight, 0.0f}, {0.3f, 0.3f, 0.3f}, {30.0f, 0.0f, 0.0f}, /*continuousCollision=*/true);

	fx.StepSeconds(0.5f);

	// Never reached, let alone passed, the character's centre line - stopped by the inner
	// body instead of sailing straight through to x = -5 + 30*0.5 = +10.
	CHECK(fx.FeetPositionOf(crate).x < 0.0f);
}

// The other half of the same class comment: "Regular collision checks (e.g.
// NarrowPhaseQuery::CastRay) will collide with the rigid body" - the physics gun's pick-up
// raycast, and any other raycast in the game, needs this to hit a character at all.
TEST_CASE("A raycast hits a character controller and returns its entity id")
{
	CharacterFixture fx;
	const aether::Entity player = fx.MakeCharacter({0.0f, 0.0f, 0.0f});
	fx.StepSeconds(aether::PhysicsSystem::kFixedTimestep); // let FlushPendingCharacters bake the body

	// Fired through the capsule's mid-height, well clear of the ground so nothing but the
	// character is in the way.
	const aether::PhysicsSystem::RaycastResult hit = fx.physics->CastRay({-5.0f, 0.9f, 0.0f}, {1.0f, 0.0f, 0.0f}, 10.0f);

	REQUIRE(hit.hit);
	CHECK(hit.entity == player.id);
}
