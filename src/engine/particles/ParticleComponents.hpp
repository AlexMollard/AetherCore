#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "scene/Components.hpp" // SpriteBlendMode

namespace aether
{
	// One live particle. Runtime-only, never serialized.
	struct Particle
	{
		glm::vec2 position{0.0f};
		glm::vec2 velocity{0.0f};
		float age = 0.0f;
		float lifetime = 1.0f;
		float sizeJitter = 1.0f; // per-particle multiplier on size
	};

	// CPU 2D particle emitter. Particles are simulated on the game thread and
	// emitted into the shared 2D sprite instance stream at extraction, so they
	// render through the existing bindless sprite pipeline with no extra pass.
	// Spawned in world space, so they outlive emitter motion (but not the
	// emitter entity - one-shot bursts should use autoDestroyWhenDone on a
	// dedicated effect entity).
	struct ParticleEmitterComponent
	{
		std::string texturePath; // particle sprite; empty = built-in white dot

		// ── Emission ──────────────────────────────────────────────────────────
		float rate = 0.0f;                    // continuous particles/sec (0 = burst only)
		std::uint32_t burstCount = 0;         // emitted on start and on each script trigger
		bool emitOnStart = false;             // fire burstCount once when first ticked
		bool emitting = true;                 // gate for continuous 'rate'
		bool autoDestroyWhenDone = false;     // destroy the entity once no particles remain
		std::uint32_t maxParticles = 256;

		// ── Per-particle spawn ────────────────────────────────────────────────
		float lifetimeMin = 0.5f;
		float lifetimeMax = 0.9f;
		float speedMin = 1.0f;
		float speedMax = 3.0f;
		float directionDeg = 90.0f; // 90 = straight up
		float spreadDeg = 30.0f;    // +/- half-angle cone around directionDeg
		glm::vec2 gravity{0.0f, -6.0f};

		// ── Appearance ────────────────────────────────────────────────────────
		float startSize = 0.3f; // world units (quad edge)
		float endSize = 0.0f;
		glm::vec4 startColor{1.0f};
		glm::vec4 endColor{1.0f, 1.0f, 1.0f, 0.0f};
		SpriteBlendMode blendMode = SpriteBlendMode::Alpha;
		std::int32_t sortingLayer = 10;
		std::int32_t orderInLayer = 0;

		// ── Collision (opt-in; both are cheap when off) ───────────────────────
		bool collideWorld = false;     // bounce off physics colliders (raycast)
		bool collideParticles = false; // resolve against sibling particles
		float bounce = 0.4f;           // restitution 0..1 on any collision
		float collisionDamping = 0.2f; // tangential velocity loss on a world hit
		float collisionRadius = 0.0f;  // world units; 0 = derive from current size

		// ── Runtime (never serialized) ────────────────────────────────────────
		std::vector<Particle> particles;
		float spawnAccumulator = 0.0f;
		std::uint32_t rngState = 0;
		bool started = false;
		std::uint32_t pendingBurst = 0; // queued by the script API
	};
} // namespace aether
