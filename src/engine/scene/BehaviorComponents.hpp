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
} // namespace aether
