#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "scene/Components.hpp" // SpriteBlendMode

namespace aether
{
	// One live particle. Runtime-only, never serialized. The 2D path uses position/velocity;
	// the 3D billboard path uses position3D/velocity3D.
	struct Particle
	{
		glm::vec2 position{0.0f};
		glm::vec2 velocity{0.0f};
		float age = 0.0f;
		float lifetime = 1.0f;
		float sizeJitter = 1.0f; // per-particle multiplier on size
		glm::vec3 position3D{0.0f};
		glm::vec3 velocity3D{0.0f};
		float rotationOffsetDeg = 0.0f; // per-particle random start angle (3D)
	};

	// Where an emitter simulates and draws. Plane2D is the original behaviour: an XY-plane
	// simulation emitted into the 2D sprite stream. Billboard3D simulates in world XYZ and
	// draws depth-tested, camera-facing quads in the 3D scene (after the opaque pass).
	enum class ParticleSpace : std::uint8_t
	{
		Plane2D = 0,
		Billboard3D,
	};

	// How a Billboard3D emitter picks spawn points and launch velocities. Box is the axis
	// jitter below. Radial/ImprovedRadial are Twinsanity's ParticleData GenSort 6 / 11: the
	// particle starts on a sphere around the emitter and flies outward along its radius.
	// In those modes spawnJitter holds the base (radius, yawDeg, polarDeg) and velocityJitter
	// the +/- random range of the same three, exactly as the disc reuses Random_Start and
	// Random_Emit. Radial measures the polar angle from straight down, negative towards up
	// (-180 = up, -90 = horizontal, 0 = down); ImprovedRadial measures it as elevation above
	// the horizontal (0 = horizontal, 90 = up).
	enum class ParticleEmitShape : std::uint8_t
	{
		Box = 0,
		Radial,
		ImprovedRadial,
	};

	// Piecewise-linear key over normalised particle age (t in 0..1). Keys are read in order
	// and the list ends at the first key whose t reaches 1, or where t goes backwards; a run of
	// keys at the same t is a jump (the later one wins going forward).
	struct ParticleScalarKey
	{
		float t = 0.0f;
		float value = 0.0f;
	};

	struct ParticleColorKey
	{
		float t = 0.0f;
		glm::vec3 color{1.0f};
	};

	// Evaluate a key list at normalised age t. Empty lists return `fallback`.
	[[nodiscard]] float EvaluateParticleKeys(const std::vector<ParticleScalarKey>& keys, float t, float fallback) noexcept;
	[[nodiscard]] glm::vec3 EvaluateParticleKeys(const std::vector<ParticleColorKey>& keys, float t, glm::vec3 fallback) noexcept;

	// CPU particle emitter. Plane2D particles are simulated on the game thread and emitted
	// into the shared 2D sprite instance stream at extraction, so they render through the
	// existing bindless sprite pipeline. Billboard3D particles are extracted into the
	// packet's billboard list and drawn by BillboardParticleRenderer.
	// Spawned in world space, so they outlive emitter motion (but not the
	// emitter entity - one-shot bursts should use autoDestroyWhenDone on a
	// dedicated effect entity).
	struct ParticleEmitterComponent
	{
		std::string texturePath; // particle sprite; empty = built-in white dot
		ParticleSpace space = ParticleSpace::Plane2D;

		// ── Emission ──────────────────────────────────────────────────────────
		float rate = 0.0f;                    // continuous particles/sec (0 = burst only)
		std::uint32_t burstCount = 0;         // emitted on start and on each script trigger
		bool emitOnStart = false;             // fire burstCount once when first ticked
		bool emitting = true;                 // gate for continuous 'rate'
		bool autoDestroyWhenDone = false;     // destroy the entity once no particles remain
		float emitDuration = 0.0f;            // seconds 'rate' runs after the first tick; 0 = forever
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

		// ── 3D billboard (space == Billboard3D) ───────────────────────────────
		// velocity3D is the base launch velocity; each axis gets +/- velocityJitter, and the
		// spawn point +/- spawnJitter around the emitter. gravity3D is an acceleration.
		glm::vec3 velocity3D{0.0f};
		glm::vec3 velocityJitter{0.0f};
		glm::vec3 spawnJitter{0.0f};
		glm::vec3 gravity3D{0.0f, -9.81f, 0.0f};
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f}; // (u0, v0, u1, v1) in the texture, v down
		float rotationJitterDeg = 0.0f;          // random start angle, +/- this
		ParticleEmitShape emitShape = ParticleEmitShape::Box;
		float radialSpeed = 0.0f;                // outward launch speed along the radius (Radial shapes)
		// Over-lifetime keys. Empty = fall back to start/end colour and size (and no spin).
		// size is the quad edge in world units; rotation is in degrees.
		std::vector<ParticleColorKey> colorKeys;
		std::vector<ParticleScalarKey> alphaKeys;
		std::vector<ParticleScalarKey> sizeKeys;
		std::vector<ParticleScalarKey> rotationKeys;
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
		float emitElapsed = 0.0f;       // seconds since the first tick (drives emitDuration)
	};
} // namespace aether
