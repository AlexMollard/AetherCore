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
