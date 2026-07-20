// Physics2D (Box2D v3) coverage. The first case exercises Box2D directly to pin
// the vendored dependency; everything else goes through Physics2DSystem.
#include <doctest/doctest.h>

#include <box2d/box2d.h>

#include "physics/PhysicsComponents.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "physics2d/SpriteColliderGen.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace
{
	// Registers a Physics2DSystem in a Scene2D world (2D physics is exclusive to
	// 2D scenes) and keeps a borrowed pointer for direct calls.
	struct Physics2DFixture
	{
		aether::World world;
		aether::Physics2DSystem* physics = nullptr;

		Physics2DFixture()
		{
			world.SetSceneKind(aether::SceneKind::Scene2D);
			world.SetSceneFeatures(aether::DefaultSceneFeatures(aether::SceneKind::Scene2D));
			auto system = std::make_unique<aether::Physics2DSystem>();
			physics = system.get();
			world.RegisterSystem(std::move(system));
		}

		aether::Entity MakeBody(glm::vec3 pos, aether::Body2DType type, aether::Collider2DComponent collider = {}, aether::RigidBody2DComponent rigid = {})
		{
			const aether::Entity e = world.Create();
			world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform(pos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			rigid.bodyType = type;
			world.Emplace<aether::RigidBody2DComponent>(e, rigid);
			world.Emplace<aether::Collider2DComponent>(e, std::move(collider));
			return e;
		}

		void StepSeconds(float seconds)
		{
			const int frames = static_cast<int>(seconds / aether::Physics2DSystem::kFixedTimestep + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				physics->Update(world, aether::Physics2DSystem::kFixedTimestep);
			}
		}

		[[nodiscard]] glm::vec3 PositionOf(aether::Entity e)
		{
			glm::vec3 pos{};
			glm::vec3 euler{};
			glm::vec3 scale{};
			aether::DecomposeTRS(world.Get<aether::TransformComponent>(e).localToWorld, pos, euler, scale);
			return pos;
		}

		[[nodiscard]] float AngleDegOf(aether::Entity e)
		{
			glm::vec3 pos{};
			glm::vec3 euler{};
			glm::vec3 scale{};
			aether::DecomposeTRS(world.Get<aether::TransformComponent>(e).localToWorld, pos, euler, scale);
			return euler.z;
		}
	};
} // namespace

TEST_CASE("Physics2D: dynamic box falls onto static ground and rests")
{
	Physics2DFixture fx;
	fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}});
	const aether::Entity faller = fx.MakeBody({0.0f, 5.0f, 3.5f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}});

	fx.StepSeconds(3.0f);

	const glm::vec3 pos = fx.PositionOf(faller);
	// Ground top at -0.5 (center -1, half-height 0.5); box half-height 0.5 -> rests at ~0.
	CHECK(pos.y == doctest::Approx(0.0f).epsilon(0.1));
	CHECK(pos.x == doctest::Approx(0.0f).epsilon(0.05));
	CHECK(pos.z == doctest::Approx(3.5f)); // transform Z is preserved through sync
	CHECK(fx.physics->GetLinearVelocity(fx.world.Get<aether::RigidBody2DComponent>(faller).body).y == doctest::Approx(0.0f).epsilon(0.1));
}

TEST_CASE("Physics2D: fixed rotation keeps a body upright through an off-center hit")
{
	Physics2DFixture fx;
	fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}});
	// Off-center static bump the faller clips with one corner.
	fx.MakeBody({0.4f, -0.2f, 0.0f}, aether::Body2DType::Static, {.size = {0.4f, 0.6f}});
	const aether::Entity faller = fx.MakeBody({0.0f, 4.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}}, {.fixedRotation = true});

	fx.StepSeconds(3.0f);

	CHECK(fx.AngleDegOf(faller) == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("Physics2D: destroying an entity mid-simulation releases its body safely")
{
	Physics2DFixture fx;
	fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}});
	const aether::Entity doomed = fx.MakeBody({0.0f, 2.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}});
	const aether::Entity survivor = fx.MakeBody({3.0f, 2.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}});

	fx.StepSeconds(0.5f);
	fx.world.Destroy(doomed);
	fx.StepSeconds(1.5f);

	CHECK(fx.PositionOf(survivor).y == doctest::Approx(0.0f).epsilon(0.1));
}

TEST_CASE("Physics2D: trigger enter and exit both fire exactly once for a pass-through")
{
	Physics2DFixture fx;
	const aether::Entity trigger = fx.MakeBody({0.0f, 0.0f, 0.0f}, aether::Body2DType::Static, {.size = {2.0f, 1.0f}, .isTrigger = true});
	const aether::Entity faller = fx.MakeBody({0.0f, 3.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {0.5f, 0.5f}});

	int triggerEnterCount = 0;
	int triggerExitCount = 0;
	int mirroredEnterCount = 0;
	int mirroredExitCount = 0;
	bool sawOverlapping = false;
	for (int i = 0; i < 240; ++i)
	{
		fx.physics->Update(fx.world, aether::Physics2DSystem::kFixedTimestep);
		if (const auto* ev = fx.world.TryGet<aether::CollisionEvents2DComponent>(trigger))
		{
			triggerEnterCount += static_cast<int>(std::count(ev->triggerEnter.begin(), ev->triggerEnter.end(), faller));
			triggerExitCount += static_cast<int>(std::count(ev->triggerExit.begin(), ev->triggerExit.end(), faller));
			sawOverlapping = sawOverlapping || std::find(ev->overlapping.begin(), ev->overlapping.end(), faller) != ev->overlapping.end();
		}
		if (const auto* ev = fx.world.TryGet<aether::CollisionEvents2DComponent>(faller))
		{
			mirroredEnterCount += static_cast<int>(std::count(ev->triggerEnter.begin(), ev->triggerEnter.end(), trigger));
			mirroredExitCount += static_cast<int>(std::count(ev->triggerExit.begin(), ev->triggerExit.end(), trigger));
		}
	}

	CHECK(triggerEnterCount == 1);
	CHECK(triggerExitCount == 1);
	CHECK(mirroredEnterCount == 1);
	CHECK(mirroredExitCount == 1);
	CHECK(sawOverlapping);
	// Fell clean through: a trigger never produces a collision response.
	CHECK(fx.PositionOf(faller).y < -2.0f);
}

TEST_CASE("Physics2D: contact begin lands on both entities and overlapping tracks the rest state")
{
	Physics2DFixture fx;
	const aether::Entity ground = fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}});
	const aether::Entity faller = fx.MakeBody({0.0f, 2.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}});

	int fallerEnterCount = 0;
	int groundEnterCount = 0;
	for (int i = 0; i < 180; ++i)
	{
		fx.physics->Update(fx.world, aether::Physics2DSystem::kFixedTimestep);
		if (const auto* ev = fx.world.TryGet<aether::CollisionEvents2DComponent>(faller))
		{
			fallerEnterCount += static_cast<int>(std::count(ev->collisionEnter.begin(), ev->collisionEnter.end(), ground));
		}
		if (const auto* ev = fx.world.TryGet<aether::CollisionEvents2DComponent>(ground))
		{
			groundEnterCount += static_cast<int>(std::count(ev->collisionEnter.begin(), ev->collisionEnter.end(), faller));
		}
	}

	CHECK(fallerEnterCount == 1);
	CHECK(groundEnterCount == 1);
	const auto* ev = fx.world.TryGet<aether::CollisionEvents2DComponent>(faller);
	REQUIRE(ev != nullptr);
	CHECK(std::find(ev->overlapping.begin(), ev->overlapping.end(), ground) != ev->overlapping.end());
}

TEST_CASE("Physics2D: ray cast reports entity, point, normal, and fraction")
{
	Physics2DFixture fx;
	const aether::Entity wall = fx.MakeBody({5.0f, 0.0f, 0.0f}, aether::Body2DType::Static, {.size = {1.0f, 4.0f}});
	fx.physics->FlushPendingOnly(fx.world);

	const auto hit = fx.physics->CastRay({0.0f, 0.0f}, {1.0f, 0.0f}, 10.0f);
	REQUIRE(hit.hit);
	CHECK(hit.entity == wall.id);
	CHECK(hit.point.x == doctest::Approx(4.5f)); // wall center 5, half-width 0.5
	CHECK(hit.point.y == doctest::Approx(0.0f));
	CHECK(hit.normal.x == doctest::Approx(-1.0f));
	CHECK(hit.fraction == doctest::Approx(0.45f));

	const auto miss = fx.physics->CastRay({0.0f, 0.0f}, {-1.0f, 0.0f}, 10.0f);
	CHECK_FALSE(miss.hit);
}

TEST_CASE("Physics2D: overlap queries find only bodies inside the region")
{
	Physics2DFixture fx;
	const aether::Entity inside = fx.MakeBody({1.0f, 1.0f, 0.0f}, aether::Body2DType::Static, {.size = {0.5f, 0.5f}});
	const aether::Entity outside = fx.MakeBody({10.0f, 10.0f, 0.0f}, aether::Body2DType::Static, {.size = {0.5f, 0.5f}});
	fx.physics->FlushPendingOnly(fx.world);

	const auto circleHits = fx.physics->OverlapCircle({1.0f, 1.0f}, 1.0f);
	CHECK(std::find(circleHits.begin(), circleHits.end(), inside.id) != circleHits.end());
	CHECK(std::find(circleHits.begin(), circleHits.end(), outside.id) == circleHits.end());

	const auto aabbHits = fx.physics->OverlapAabb({0.0f, 0.0f}, {2.0f, 2.0f});
	CHECK(std::find(aabbHits.begin(), aabbHits.end(), inside.id) != aabbHits.end());
	CHECK(std::find(aabbHits.begin(), aabbHits.end(), outside.id) == aabbHits.end());

	CHECK_FALSE(fx.physics->OverlapPoint({1.0f, 1.0f}).empty());
	CHECK(fx.physics->OverlapPoint({5.0f, 5.0f}).empty());
}

TEST_CASE("Physics2D: circle cast stops at the first obstacle")
{
	Physics2DFixture fx;
	const aether::Entity wall = fx.MakeBody({6.0f, 0.0f, 0.0f}, aether::Body2DType::Static, {.size = {1.0f, 6.0f}});
	fx.physics->FlushPendingOnly(fx.world);

	const auto hit = fx.physics->CastCircle({0.0f, 0.0f}, 0.5f, {1.0f, 0.0f}, 10.0f);
	REQUIRE(hit.hit);
	CHECK(hit.entity == wall.id);
	// Circle surface meets the wall face at x=5.5 (wall left face), center travels 5.0.
	CHECK(hit.fraction == doctest::Approx(0.5f).epsilon(0.02));

	const auto miss = fx.physics->CastCircle({0.0f, 0.0f}, 0.5f, {0.0f, 1.0f}, 10.0f);
	CHECK_FALSE(miss.hit);
}

namespace
{
	// A mixed scene: ground, a trigger, and a spread of boxes/circles/capsules
	// with varied restitution. Returns the dynamic entities in creation order.
	std::vector<aether::Entity> BuildReplayScene(Physics2DFixture& fx)
	{
		fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {40.0f, 1.0f}});
		fx.MakeBody({0.0f, 1.0f, 0.0f}, aether::Body2DType::Static, {.size = {1.5f, 1.5f}, .isTrigger = true});

		std::vector<aether::Entity> dynamics;
		for (int i = 0; i < 10; ++i)
		{
			const float x = -9.0f + 2.0f * static_cast<float>(i);
			const float restitution = 0.05f * static_cast<float>(i);
			dynamics.push_back(fx.MakeBody({x, 4.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {0.8f, 0.6f}, .restitution = restitution}));
			dynamics.push_back(fx.MakeBody({x + 0.3f, 7.0f, 0.0f}, aether::Body2DType::Dynamic, {.shape = aether::Collider2DShape::Circle, .radius = 0.4f, .restitution = restitution}));
			dynamics.push_back(fx.MakeBody({x - 0.2f, 10.0f, 0.0f}, aether::Body2DType::Dynamic, {.shape = aether::Collider2DShape::Capsule, .radius = 0.25f, .capsuleHeight = 1.2f, .restitution = 0.5f - restitution}));
		}
		return dynamics;
	}
} // namespace

TEST_CASE("Physics2D: identical scenes replay bitwise-identically over 600 fixed steps")
{
	Physics2DFixture runA;
	Physics2DFixture runB;
	const std::vector<aether::Entity> bodiesA = BuildReplayScene(runA);
	const std::vector<aether::Entity> bodiesB = BuildReplayScene(runB);
	REQUIRE(bodiesA.size() == bodiesB.size());

	for (int i = 0; i < 600; ++i)
	{
		runA.physics->Update(runA.world, aether::Physics2DSystem::kFixedTimestep);
		runB.physics->Update(runB.world, aether::Physics2DSystem::kFixedTimestep);
	}

	for (std::size_t i = 0; i < bodiesA.size(); ++i)
	{
		const glm::vec3 posA = runA.PositionOf(bodiesA[i]);
		const glm::vec3 posB = runB.PositionOf(bodiesB[i]);
		// Bitwise equality, not Approx: Box2D v3.1 is deterministic for
		// identical inputs on the same binary.
		CHECK(posA.x == posB.x);
		CHECK(posA.y == posB.y);
		CHECK(runA.AngleDegOf(bodiesA[i]) == runB.AngleDegOf(bodiesB[i]));
	}
}

TEST_CASE("Physics2D: continuous collision keeps a fast body from tunnelling a thin wall")
{
	Physics2DFixture fx;
	fx.MakeBody({10.0f, 0.0f, 0.0f}, aether::Body2DType::Static, {.size = {0.1f, 10.0f}});
	const aether::Entity bullet = fx.MakeBody({0.0f, 0.0f, 0.0f}, aether::Body2DType::Dynamic,
	        {.shape = aether::Collider2DShape::Circle, .radius = 0.1f},
	        {.gravityScale = 0.0f, .continuousCollision = true, .initialVelocity = {150.0f, 0.0f}});

	fx.StepSeconds(1.0f);

	CHECK(fx.PositionOf(bullet).x < 10.0f); // stopped at the wall, not beyond it
}

TEST_CASE("Physics2D: disjoint collision filters pass through each other")
{
	Physics2DFixture fx;
	// Ground only collides with category 2; the faller is category 4 masked to 4.
	fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}, .categoryBits = 0x2u, .maskBits = 0x2u});
	const aether::Entity ghost = fx.MakeBody({0.0f, 2.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}, .categoryBits = 0x4u, .maskBits = 0x4u});

	fx.StepSeconds(2.0f);

	CHECK(fx.PositionOf(ghost).y < -3.0f); // fell straight through the ground
}

TEST_CASE("Physics2D: zero gravity scale leaves a body hanging")
{
	Physics2DFixture fx;
	const aether::Entity hanging = fx.MakeBody({0.0f, 5.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}}, {.gravityScale = 0.0f});

	fx.StepSeconds(2.0f);

	CHECK(fx.PositionOf(hanging).y == doctest::Approx(5.0f));
}

TEST_CASE("Physics2D: distance joint holds two bodies at rest length")
{
	Physics2DFixture fx;
	const aether::Entity anchor = fx.MakeBody({0.0f, 5.0f, 0.0f}, aether::Body2DType::Static, {.size = {0.5f, 0.5f}});
	const aether::Entity bob = fx.MakeBody({0.0f, 3.0f, 0.0f}, aether::Body2DType::Dynamic, {.shape = aether::Collider2DShape::Circle, .radius = 0.25f});

	aether::Joint2DComponent joint;
	joint.type = aether::Joint2DType::Distance;
	joint.target = anchor;
	joint.length = 2.0f;
	fx.world.Emplace<aether::Joint2DComponent>(bob, joint);

	fx.StepSeconds(3.0f);

	const glm::vec3 anchorPos = fx.PositionOf(anchor);
	const glm::vec3 bobPos = fx.PositionOf(bob);
	const float distance = glm::length(glm::vec2(bobPos) - glm::vec2(anchorPos));
	CHECK(distance == doctest::Approx(2.0f).epsilon(0.05));
	CHECK(fx.world.Get<aether::Joint2DComponent>(bob).jointId != 0);
}

TEST_CASE("Physics2D: revolute motor spins a body with the expected sign")
{
	Physics2DFixture fx;
	const aether::Entity pivot = fx.MakeBody({0.0f, 0.0f, 0.0f}, aether::Body2DType::Static, {.shape = aether::Collider2DShape::Circle, .radius = 0.1f});
	const aether::Entity wheel = fx.MakeBody({0.0f, 0.0f, 0.0f}, aether::Body2DType::Dynamic,
	        {.shape = aether::Collider2DShape::Circle, .radius = 0.5f, .categoryBits = 0x2u, .maskBits = 0x4u},
	        {.gravityScale = 0.0f});

	aether::Joint2DComponent joint;
	joint.type = aether::Joint2DType::Revolute;
	joint.target = pivot;
	joint.enableMotor = true;
	joint.motorSpeed = 2.0f; // radians/s, +CCW
	joint.maxMotorForce = 100.0f;
	fx.world.Emplace<aether::Joint2DComponent>(wheel, joint);

	fx.StepSeconds(0.5f);

	const auto& rigid = fx.world.Get<aether::RigidBody2DComponent>(wheel);
	CHECK(fx.physics->GetAngularVelocity(rigid.body) == doctest::Approx(2.0f).epsilon(0.05));
}

TEST_CASE("Physics2D: destroying a joint target releases the joint without crashing")
{
	Physics2DFixture fx;
	fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}});
	const aether::Entity anchor = fx.MakeBody({0.0f, 5.0f, 0.0f}, aether::Body2DType::Static, {.size = {0.5f, 0.5f}});
	const aether::Entity bob = fx.MakeBody({0.0f, 3.0f, 0.0f}, aether::Body2DType::Dynamic, {.shape = aether::Collider2DShape::Circle, .radius = 0.25f});

	aether::Joint2DComponent joint;
	joint.type = aether::Joint2DType::Distance;
	joint.target = anchor;
	joint.length = 2.0f;
	fx.world.Emplace<aether::Joint2DComponent>(bob, joint);

	fx.StepSeconds(1.0f);
	fx.world.Destroy(anchor);
	fx.StepSeconds(2.0f);

	// With the anchor gone the bob falls to the ground and rests.
	CHECK(fx.PositionOf(bob).y == doctest::Approx(-0.25f).epsilon(0.2));
}

TEST_CASE("Physics2D: the system simulates in every scene kind (Unity-style)")
{
	aether::World world; // defaults to Scene3D - 2D physics must still work
	world.RegisterSystem(std::make_unique<aether::Physics2DSystem>());

	const aether::Entity e = world.Create();
	world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{.localToWorld = aether::ComposeTransform({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	world.Emplace<aether::RigidBody2DComponent>(e);
	world.Emplace<aether::Collider2DComponent>(e);

	for (int i = 0; i < 30; ++i)
	{
		world.UpdateSystems(aether::Physics2DSystem::kFixedTimestep);
	}

	CHECK(world.Get<aether::RigidBody2DComponent>(e).body.IsValid());
	glm::vec3 pos{}, euler{}, scale{};
	aether::DecomposeTRS(world.Get<aether::TransformComponent>(e).localToWorld, pos, euler, scale);
	CHECK(pos.y < 5.0f); // gravity acted: hybrid 2D-in-3D scenes simulate
}

TEST_CASE("Physics2D: an entity carrying 3D physics components is skipped with its 2D body uncreated")
{
	Physics2DFixture fx;
	const aether::Entity mixed = fx.MakeBody({0.0f, 5.0f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}});
	fx.world.Emplace<aether::RigidBodyComponent>(mixed); // cross-domain contamination

	fx.StepSeconds(0.5f);

	CHECK_FALSE(fx.world.Get<aether::RigidBody2DComponent>(mixed).body.IsValid());
}

TEST_CASE("Sprite outline converts to pivot-relative collider points")
{
	// 32x32 sprite, ppu = 32, centred pivot: the full-rect outline maps to the
	// unit box. Pixel origin is top-left (+y down); pivot is bottom-left based,
	// matching the sprite quad's corner space.
	const glm::vec2 outline[] = {{0.0f, 0.0f}, {32.0f, 0.0f}, {32.0f, 32.0f}, {0.0f, 32.0f}};
	const auto centered = aether::BuildColliderPointsFromOutline(outline, {32.0f, 32.0f}, {0.5f, 0.5f}, 32.0f);
	REQUIRE(centered.size() == 4);
	CHECK(centered[0] == glm::vec2{-0.5f, 0.5f});  // top-left pixel -> upper-left in world
	CHECK(centered[1] == glm::vec2{0.5f, 0.5f});
	CHECK(centered[2] == glm::vec2{0.5f, -0.5f});
	CHECK(centered[3] == glm::vec2{-0.5f, -0.5f});

	// Bottom-centre pivot (0.5, 0): the sprite's bottom row sits at y = 0.
	const auto bottomPivot = aether::BuildColliderPointsFromOutline(outline, {32.0f, 32.0f}, {0.5f, 0.0f}, 32.0f);
	REQUIRE(bottomPivot.size() == 4);
	CHECK(bottomPivot[0].y == doctest::Approx(1.0f));
	CHECK(bottomPivot[2].y == doctest::Approx(0.0f));

	// Degenerate outlines produce nothing.
	CHECK(aether::BuildColliderPointsFromOutline({}, {32.0f, 32.0f}, {0.5f, 0.5f}, 32.0f).empty());
}

TEST_CASE("Physics2D: a generated polygon collider simulates")
{
	Physics2DFixture fx;
	fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}});

	// Triangle outline on a 32x32 sprite (ppu 32, centred pivot).
	const glm::vec2 outline[] = {{16.0f, 0.0f}, {32.0f, 32.0f}, {0.0f, 32.0f}};
	aether::Collider2DComponent collider;
	collider.shape = aether::Collider2DShape::Polygon;
	collider.points = aether::BuildColliderPointsFromOutline(outline, {32.0f, 32.0f}, {0.5f, 0.5f}, 32.0f);
	REQUIRE(collider.points.size() == 3);
	const aether::Entity faller = fx.MakeBody({0.0f, 4.0f, 0.0f}, aether::Body2DType::Dynamic, std::move(collider));

	fx.StepSeconds(3.0f);

	// Rests with the triangle's base (half a unit below the pivot) on the ground top (-0.5).
	CHECK(fx.PositionOf(faller).y == doctest::Approx(0.0f).epsilon(0.15));
}

TEST_CASE("Physics2D: kinematic bodies follow authored transforms")
{
	Physics2DFixture fx;
	const aether::Entity platform = fx.MakeBody({0.0f, 0.0f, 0.0f}, aether::Body2DType::Kinematic, {.size = {2.0f, 0.5f}});
	fx.StepSeconds(0.1f); // create the body

	fx.world.Get<aether::TransformComponent>(platform).localToWorld = aether::ComposeTransform({4.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
	fx.StepSeconds(0.1f);

	const auto& rigid = fx.world.Get<aether::RigidBody2DComponent>(platform);
	CHECK(rigid.body.IsValid());
	// The Box2D body picked up the authored pose (verified through a query).
	const auto hits = fx.physics->OverlapPoint({4.0f, 1.0f});
	CHECK(std::find(hits.begin(), hits.end(), platform.id) != hits.end());
}

TEST_CASE("Physics2D: setting velocity on a sleeping body wakes and moves it")
{
	// Box2D v3's b2Body_SetLinearVelocity does NOT wake a sleeping body, so a
	// script-driven actor that idles long enough to sleep would freeze forever.
	// Our wrapper adds Unity semantics: a non-zero velocity write revives it.
	Physics2DFixture fx;
	fx.MakeBody({0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {40.0f, 1.0f}});
	const aether::Entity walker = fx.MakeBody({0.0f, 0.05f, 0.0f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}});

	// Let it settle and fall asleep (sleep threshold is ~0.5s of rest).
	fx.StepSeconds(2.0f);
	const auto body = fx.world.Get<aether::RigidBody2DComponent>(walker).body;
	REQUIRE_FALSE(fx.physics->IsBodyAwake(body));

	fx.physics->SetLinearVelocity(body, {3.0f, 0.0f});
	fx.StepSeconds(1.0f);

	// A single velocity write, then friction decays it - any significant travel
	// proves the sleeping body woke (asleep it stays exactly at 0).
	CHECK(fx.PositionOf(walker).x > 0.5f);
}

TEST_CASE("Physics2D: ray casts pass through triggers and hit solid geometry behind them")
{
	Physics2DFixture fx;
	// Trigger at x=2, solid wall at x=4, ray fired from origin along +X.
	fx.MakeBody({2.0f, 0.0f, 0.0f}, aether::Body2DType::Static, {.size = {1.0f, 4.0f}, .isTrigger = true});
	const aether::Entity wall = fx.MakeBody({4.0f, 0.0f, 0.0f}, aether::Body2DType::Static, {.size = {1.0f, 4.0f}});
	fx.StepSeconds(0.1f);

	const auto hit = fx.physics->CastRay({0.0f, 0.0f}, {1.0f, 0.0f}, 10.0f);
	REQUIRE(hit.hit);
	CHECK(hit.entity == wall.id);
	CHECK(hit.point.x == doctest::Approx(3.5f).epsilon(0.01));
}

TEST_CASE("Box2D world steps and a dynamic body falls")
{
	b2WorldDef worldDef = b2DefaultWorldDef();
	worldDef.gravity = {0.0f, -10.0f};
	const b2WorldId world = b2CreateWorld(&worldDef);

	b2BodyDef bodyDef = b2DefaultBodyDef();
	bodyDef.type = b2_dynamicBody;
	bodyDef.position = {0.0f, 10.0f};
	const b2BodyId body = b2CreateBody(world, &bodyDef);
	const b2Polygon box = b2MakeBox(0.5f, 0.5f);
	b2ShapeDef shapeDef = b2DefaultShapeDef();
	b2CreatePolygonShape(body, &shapeDef, &box);

	for (int i = 0; i < 60; ++i)
	{
		b2World_Step(world, 1.0f / 60.0f, 4);
	}

	CHECK(b2Body_GetPosition(body).y < 5.0f);
	b2DestroyWorld(world);
}
