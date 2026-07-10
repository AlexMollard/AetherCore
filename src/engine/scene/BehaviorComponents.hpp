#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace aether
{
	// Small data-driven scene behaviors, advanced by the app's BehaviorSystem
	// while the simulation is playing. They replace the per-frame animation
	// code that used to live in the scene script's on_update - as plain
	// components they serialize with the scene and survive the planned
	// script-language migration.

	// Sinusoidal vertical bob around the Y captured on the first update.
	struct BobComponent
	{
		float amplitude = 1.0f;
		float frequency = 1.0f; // radians/sec multiplier inside sin(t*f + phase)
		float phase = 0.0f;
		bool baseCaptured = false;
		float baseY = 0.0f;
		float time = 0.0f;
	};

	// Constant euler rotation (degrees per second) applied around the entity's
	// current position, preserving translation and scale.
	struct SpinComponent
	{
		glm::vec3 eulerDegPerSec{0.0f, 40.0f, 0.0f};
	};

	// Circular patrol around a world-space center, facing along the tangent.
	struct OrbitComponent
	{
		glm::vec3 center{0.0f};
		float radius = 5.0f;
		float angularSpeedDeg = 30.0f; // degrees/sec around +Y
		float angleDeg = 0.0f;         // current angle (serialized so loads resume in place)
		float yawOffsetDeg = 0.0f;     // model-forward correction
		float height = 0.0f;           // Y of the orbit plane
	};

	// Ping-pongs the material emissive between two colors. Runs through the
	// per-entity material-instance setters, so identical pulses on multiple
	// entities dedup to a single registry slot per step - the Zone-M shared
	// material demo semantics, now data-driven.
	struct MaterialPulseComponent
	{
		glm::vec3 emissiveA{0.0f};
		glm::vec3 emissiveB{1.0f, 0.5f, 0.1f};
		float frequency = 2.0f; // radians/sec inside sin
		float time = 0.0f;
	};

	// Sinusoidal uniform-scale "breathing" around the scale captured on the first
	// update (so a gizmo scale while paused re-bases naturally on re-apply).
	// Position and rotation are preserved.
	struct ScalePulseComponent
	{
		float amplitude = 0.2f; // peak fractional change (0.2 = +/-20% of base)
		float frequency = 2.0f; // radians/sec inside sin
		float phase = 0.0f;
		bool baseCaptured = false;
		glm::vec3 baseScale{1.0f};
		float time = 0.0f;
	};

	// Continuously aims the entity's local -Z at a world-space target point
	// (camera-forward convention), preserving position and scale. Handy for
	// pointing a camera or spot light at a fixed spot in the scene.
	struct LookAtComponent
	{
		glm::vec3 target{0.0f};
		bool keepUpright = true; // lock roll using world +Y as up
	};
} // namespace aether
