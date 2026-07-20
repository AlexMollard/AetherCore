#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace aether
{
	// components they serialize with the scene and survive the planned

	struct BobComponent
	{
		float amplitude = 1.0f;
		float frequency = 1.0f;
		float phase = 0.0f;
		bool baseCaptured = false;
		float baseY = 0.0f;
		float time = 0.0f;
	};

	struct SpinComponent
	{
		glm::vec3 eulerDegPerSec{0.0f, 40.0f, 0.0f};
	};

	struct OrbitComponent
	{
		glm::vec3 center{0.0f};
		float radius = 5.0f;
		float angularSpeedDeg = 30.0f;
		float angleDeg = 0.0f;
		float yawOffsetDeg = 0.0f;
		float height = 0.0f;
	};

	struct MaterialPulseComponent
	{
		glm::vec3 emissiveA{0.0f};
		glm::vec3 emissiveB{1.0f, 0.5f, 0.1f};
		float frequency = 2.0f;
		float time = 0.0f;
	};

	struct ScalePulseComponent
	{
		float amplitude = 0.2f;
		float frequency = 2.0f;
		float phase = 0.0f;
		bool baseCaptured = false;
		glm::vec3 baseScale{1.0f};
		float time = 0.0f;
	};

	struct LookAtComponent
	{
		glm::vec3 target{0.0f};
		bool keepUpright = true; // lock roll using world +Y as up
	};

	// Parallax scrolling for 2D background/foreground layers. Each frame the
	// layer is repositioned relative to the main camera so it appears to scroll
	// at a fraction of the camera's speed:
	//   factor = 1 -> moves with the world (gameplay plane, no parallax)
	//   factor = 0 -> locked to the camera (infinitely distant, e.g. the sky)
	//   0 < factor < 1 -> distant background (slow); factor > 1 -> foreground (fast)
	// scrollSpeed adds a constant world-units/sec drift (e.g. clouds sliding).
	struct ParallaxComponent
	{
		glm::vec2 factor{0.5f, 1.0f};
		glm::vec2 scrollSpeed{0.0f};
		// Runtime: base anchor captured on first tick so Play never jumps, plus
		// the accumulated drift clock. Not serialized.
		bool baseCaptured = false;
		glm::vec2 base{0.0f};
		float time = 0.0f;
	};
} // namespace aether
