#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "particles/ParticleComponents.hpp"
#include "particles/ParticleSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;

namespace
{
	ParticleEmitterComponent BurstEmitter(std::uint32_t count, float lifetime)
	{
		ParticleEmitterComponent e;
		e.rate = 0.0f;
		e.burstCount = count;
		e.emitOnStart = true;
		e.lifetimeMin = lifetime;
		e.lifetimeMax = lifetime;
		e.speedMin = 1.0f;
		e.speedMax = 1.0f;
		return e;
	}
} // namespace

TEST_CASE("emit-on-start burst spawns exactly once and ages out")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	ParticleSystem particles(textures);

	World world;
	const Entity e = world.Create();
	world.Emplace<TransformComponent>(e);
	world.Emplace<ParticleEmitterComponent>(e, BurstEmitter(5, 0.1f));

	// First tick fires the start burst.
	particles.Update(world, 0.016f);
	CHECK(world.Get<ParticleEmitterComponent>(e).particles.size() == 5);

	// A second tick must NOT re-fire the start burst.
	particles.Update(world, 0.016f);
	CHECK(world.Get<ParticleEmitterComponent>(e).particles.size() == 5);

	// Extraction turns each live particle into one instance.
	Render2DFrameData frame;
	particles.Extract(world, frame);
	CHECK(frame.sprites.size() == 5);

	// Past the 0.1s lifetime every particle is culled.
	particles.Update(world, 0.2f);
	CHECK(world.Get<ParticleEmitterComponent>(e).particles.empty());
}

TEST_CASE("queued burst emits and respects the max-particles cap")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	ParticleSystem particles(textures);

	World world;
	const Entity e = world.Create();
	world.Emplace<TransformComponent>(e);
	ParticleEmitterComponent cfg = BurstEmitter(0, 1.0f);
	cfg.emitOnStart = false;
	cfg.maxParticles = 8;
	world.Emplace<ParticleEmitterComponent>(e, cfg);

	ParticleSystem::QueueBurst(world.Get<ParticleEmitterComponent>(e), 20);
	particles.Update(world, 0.016f);
	// Capped at maxParticles despite requesting 20.
	CHECK(world.Get<ParticleEmitterComponent>(e).particles.size() == 8);
}

TEST_CASE("particle-particle collision pushes overlapping siblings apart")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	ParticleSystem particles(textures);

	World world;
	const Entity e = world.Create();
	world.Emplace<TransformComponent>(e);
	ParticleEmitterComponent cfg = BurstEmitter(0, 100.0f);
	cfg.emitOnStart = false;
	cfg.gravity = {0.0f, 0.0f};
	cfg.collideParticles = true;
	cfg.collisionRadius = 0.5f; // pair should sit 1.0 apart
	cfg.bounce = 0.5f;
	world.Emplace<ParticleEmitterComponent>(e, cfg);

	// Two stationary particles overlapping (0.3 apart, want 1.0).
	auto& live = world.Get<ParticleEmitterComponent>(e).particles;
	Particle a;
	a.position = {0.0f, 0.0f};
	a.lifetime = 100.0f;
	a.sizeJitter = 1.0f;
	Particle b = a;
	b.position = {0.3f, 0.0f};
	live = {a, b};

	particles.Update(world, 0.016f);

	const auto& out = world.Get<ParticleEmitterComponent>(e).particles;
	REQUIRE(out.size() == 2);
	const float dist = glm::length(out[0].position - out[1].position);
	CHECK(dist > 0.9f); // separated toward the 1.0 rest distance
}

TEST_CASE("one-shot emitter self-destructs when finished")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	ParticleSystem particles(textures);

	World world;
	const Entity e = world.Create();
	world.Emplace<TransformComponent>(e);
	ParticleEmitterComponent cfg = BurstEmitter(4, 0.1f);
	cfg.autoDestroyWhenDone = true;
	world.Emplace<ParticleEmitterComponent>(e, cfg);

	particles.Update(world, 0.016f); // burst
	CHECK(world.GetRegistry().valid(World::ToEntt(e)));

	particles.Update(world, 0.2f); // particles die -> entity retired
	CHECK_FALSE(world.GetRegistry().valid(World::ToEntt(e)));
}

// ── Key gradient maths ─────────────────────────────────────────────────────────

TEST_CASE("scalar keys interpolate piecewise-linearly and clamp past the terminal key")
{
	// The disc gradients read in order and end at the first t == 1: [0, 0.25, 1] over
	// [0, 255, 0] rises then falls, and keys AFTER the terminal 1 are leftovers.
	const std::vector<ParticleScalarKey> keys{{0.0f, 0.0f}, {0.25f, 255.0f}, {1.0f, 0.0f}, {1.0f, 99.0f}};
	CHECK(EvaluateParticleKeys(keys, 0.0f, 1.0f) == doctest::Approx(0.0f));
	CHECK(EvaluateParticleKeys(keys, 0.125f, 1.0f) == doctest::Approx(127.5f));
	CHECK(EvaluateParticleKeys(keys, 0.25f, 1.0f) == doctest::Approx(255.0f));
	CHECK(EvaluateParticleKeys(keys, 0.625f, 1.0f) == doctest::Approx(127.5f));
	CHECK(EvaluateParticleKeys(keys, 1.0f, 1.0f) == doctest::Approx(0.0f));
	CHECK(EvaluateParticleKeys(keys, 0.7f, 1.0f) == doctest::Approx(255.0f * (1.0f - (0.7f - 0.25f) / 0.75f)));
}

TEST_CASE("duplicate keys at one t form a step; empty keys fall back")
{
	// The disc's alpha tracks often delay the visible ramp with a duplicated t=0 key:
	// (0,0),(0,255) holds 0 AT t=0 and jumps to 255 as soon as t moves.
	const std::vector<ParticleScalarKey> step{{0.0f, 0.0f}, {0.0f, 255.0f}, {1.0f, 255.0f}};
	CHECK(EvaluateParticleKeys(step, 0.0f, 0.0f) == doctest::Approx(0.0f));
	CHECK(EvaluateParticleKeys(step, 0.01f, 0.0f) == doctest::Approx(255.0f));

	const std::vector<ParticleScalarKey> none;
	CHECK(EvaluateParticleKeys(none, 0.5f, 7.0f) == doctest::Approx(7.0f));
}

TEST_CASE("colour keys interpolate each channel independently")
{
	const std::vector<ParticleColorKey> keys{{0.0f, glm::vec3{1.0f, 0.0f, 0.5f}}, {1.0f, glm::vec3{0.0f, 1.0f, 0.5f}}};
	const glm::vec3 mid = EvaluateParticleKeys(keys, 0.5f, glm::vec3{0.0f});
	CHECK(mid.x == doctest::Approx(0.5f));
	CHECK(mid.y == doctest::Approx(0.5f));
	CHECK(mid.z == doctest::Approx(0.5f)); // equal channel stays put
}

// ── 3D billboard simulation ────────────────────────────────────────────────────

TEST_CASE("billboard particles spawn inside the jitter box and fall under gravity3D")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	ParticleSystem particles(textures);

	World world;
	const Entity e = world.Create();
	TransformComponent t;
	t.localToWorld[3] = glm::vec4{10.0f, 5.0f, -3.0f, 1.0f};
	world.Emplace<TransformComponent>(e, t);
	ParticleEmitterComponent cfg = BurstEmitter(16, 1.0f);
	cfg.space = ParticleSpace::Billboard3D;
	cfg.velocity3D = glm::vec3{0.0f};
	cfg.velocityJitter = glm::vec3{2.0f, 0.0f, 2.0f};
	cfg.spawnJitter = glm::vec3{0.5f, 0.5f, 0.5f};
	cfg.gravity3D = glm::vec3{0.0f, -10.0f, 0.0f};
	cfg.sizeKeys = {{0.0f, 1.0f}, {1.0f, 0.0f}};
	world.Emplace<ParticleEmitterComponent>(e, cfg);

	particles.Update(world, 0.0f); // deterministic RNG seeding only
	auto& emitter = world.Get<ParticleEmitterComponent>(e);
	particles.Update(world, 0.016f);

	REQUIRE(emitter.particles.size() == 16);
	for (const Particle& p: emitter.particles)
	{
		// Spawn point inside emitter +/- spawnJitter.
		CHECK(p.position3D.x >= 9.5f - 1e-4f);
		CHECK(p.position3D.x <= 10.5f + 1e-4f);
		CHECK(std::abs(p.position3D.y - 5.0f) <= 0.5f + 1e-4f);
		// Launch velocity = velocity3D +/- jitter per axis; one tick of g = -10 is folded in
		// because Update integrates before we look.
		CHECK(std::abs(p.velocity3D.x) <= 2.0f + 1e-4f);
		CHECK(p.velocity3D.y == doctest::Approx(-10.0f * 0.016f).epsilon(0.01f));
	}
	// Half a second of fall: y velocity picked up -10 * 0.5 on the last particle checked
	// after exactly one tick it is just the spawn velocity minus one tick of gravity.
	const Particle& first = emitter.particles.front();
	CHECK(first.velocity3D.y < 0.0f);

	// Extraction feeds the billboard stream, not the 2D sprite stream, with the size
	// gradient applied (t=0 -> 1.0 world-unit edge) and the entity id carried.
	Render2DFrameData frame;
	particles.SetBillboardTarget(&frame.billboards);
	particles.Extract(world, frame);
	particles.SetBillboardTarget(nullptr);
	CHECK(frame.sprites.empty());
	REQUIRE(frame.billboards.size() == 16);
	CHECK(frame.billboards[0].positionSize.w == doctest::Approx(1.0f).epsilon(0.05f)); // t~0.016 of the 1->0 size ramp
	CHECK(frame.billboards[0].entityId == e.id);
}

TEST_CASE("radial shapes spawn on the disc's sphere and fly out along the radius")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	ParticleSystem particles(textures);

	World world;
	const Entity e = world.Create();
	world.Emplace<TransformComponent>(e, TransformComponent{});
	ParticleEmitterComponent cfg = BurstEmitter(32, 1.0f);
	cfg.space = ParticleSpace::Billboard3D;
	cfg.gravity3D = glm::vec3{0.0f};
	cfg.emitShape = ParticleEmitShape::Radial;
	// CRASH_DROP2-style ring: radius 1.5, full circle of yaw, polar -90 (horizontal).
	cfg.spawnJitter = glm::vec3{1.5f, 180.0f, -90.0f};
	cfg.velocityJitter = glm::vec3{0.0f, 180.0f, 0.0f};
	cfg.radialSpeed = 4.0f;
	world.Emplace<ParticleEmitterComponent>(e, cfg);

	particles.Update(world, 0.0f);
	auto& emitter = world.Get<ParticleEmitterComponent>(e);
	particles.Update(world, 0.001f);
	REQUIRE(emitter.particles.size() == 32);
	bool sawNegX = false, sawPosX = false;
	for (const Particle& p: emitter.particles)
	{
		CHECK(std::abs(p.position3D.y) < 0.01f);                           // a flat ring, not a ball
		CHECK(glm::length(p.position3D) == doctest::Approx(1.5f).epsilon(0.01f)); // on the base radius
		CHECK(glm::length(p.velocity3D) == doctest::Approx(4.0f).epsilon(0.001f));
		CHECK(glm::dot(glm::normalize(p.position3D), glm::normalize(p.velocity3D)) == doctest::Approx(1.0f).epsilon(0.001f)); // outward
		sawNegX = sawNegX || p.position3D.x < -0.5f;
		sawPosX = sawPosX || p.position3D.x > 0.5f;
	}
	CHECK((sawNegX && sawPosX)); // +/-180 of yaw covers the whole circle

	// Radial polar 0 is straight up and -84.6 (CRASH_DROP2) lifts off the ground; ImprovedRadial
	// measures elevation, so 90 is up.
	emitter.particles.clear();
	emitter.spawnJitter = glm::vec3{0.0f, 0.0f, 0.0f};
	emitter.velocityJitter = glm::vec3{0.0f};
	emitter.pendingBurst = 1;
	particles.Update(world, 0.001f);
	REQUIRE(emitter.particles.size() == 1);
	CHECK(emitter.particles[0].velocity3D.y == doctest::Approx(4.0f).epsilon(0.001f));
	emitter.particles.clear();
	emitter.spawnJitter = glm::vec3{0.0f, 0.0f, -84.6f};
	emitter.pendingBurst = 1;
	particles.Update(world, 0.001f);
	REQUIRE(emitter.particles.size() == 1);
	CHECK(emitter.particles[0].velocity3D.y > 0.0f);
	emitter.particles.clear();
	emitter.emitShape = ParticleEmitShape::ImprovedRadial;
	emitter.spawnJitter = glm::vec3{0.0f, 0.0f, 90.0f};
	emitter.pendingBurst = 1;
	particles.Update(world, 0.001f);
	REQUIRE(emitter.particles.size() == 1);
	CHECK(emitter.particles[0].velocity3D.y == doctest::Approx(4.0f).epsilon(0.001f));
}

TEST_CASE("emit_duration stops the rate stream but lets live particles finish")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	ParticleSystem particles(textures);

	World world;
	const Entity e = world.Create();
	world.Emplace<TransformComponent>(e);
	ParticleEmitterComponent cfg = BurstEmitter(0, 0.05f);
	cfg.emitOnStart = false;
	cfg.rate = 1000.0f; // 10 per 10 ms tick
	cfg.emitDuration = 0.03f;
	world.Emplace<ParticleEmitterComponent>(e, cfg);

	particles.Update(world, 0.01f);
	particles.Update(world, 0.01f);
	particles.Update(world, 0.01f);
	CHECK(world.Get<ParticleEmitterComponent>(e).particles.size() == 30);

	const std::size_t before = world.Get<ParticleEmitterComponent>(e).particles.size();
	particles.Update(world, 0.01f); // past emit_duration: nothing new spawns...
	CHECK(world.Get<ParticleEmitterComponent>(e).particles.size() <= before);
	particles.Update(world, 0.1f); // ...and the short-lived ones die out.
	CHECK(world.Get<ParticleEmitterComponent>(e).particles.empty());
}
