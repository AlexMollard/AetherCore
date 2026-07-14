#pragma once

#if defined(__clang__)
#	pragma clang diagnostic ignored "-Wreturn-type-c-linkage" // POD value returns are part of the fixed managed ABI.
#endif

#include <cstdint>

#include <glm/glm.hpp>

#include "scripting/SceneContext.hpp"

// The ABI is deliberately blittable: entity ids are uint32, vectors are Vec3

#ifdef _WIN32
#	define AE_SCRIPT_API extern "C" __declspec(dllexport)
#else
#	define AE_SCRIPT_API extern "C" __attribute__((visibility("default")))
#endif

namespace aether::app::scripting::interop
{
	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	static_assert(sizeof(Vec3) == 12, "Vec3 must be 3 tightly-packed floats to match Vector3/glm::vec3");
	static_assert(sizeof(glm::vec3) == sizeof(Vec3), "glm::vec3 layout must match interop Vec3");

	struct Vec2
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	struct Vec4
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float w = 0.0f;
	};

	static_assert(sizeof(Vec2) == 8, "Vec2 must be 2 tightly-packed floats to match Vector2/glm::vec2");
	static_assert(sizeof(Vec4) == 16, "Vec4 must be 4 tightly-packed floats to match Vector4/glm::vec4");

	[[nodiscard]] inline glm::vec3 ToGlm(Vec3 v) noexcept
	{
		return {v.x, v.y, v.z};
	}

	[[nodiscard]] inline Vec3 FromGlm(const glm::vec3& v) noexcept
	{
		return {v.x, v.y, v.z};
	}

	[[nodiscard]] inline glm::vec2 ToGlm(Vec2 v) noexcept
	{
		return {v.x, v.y};
	}

	[[nodiscard]] inline glm::vec4 ToGlm(Vec4 v) noexcept
	{
		return {v.x, v.y, v.z, v.w};
	}

	[[nodiscard]] inline aether::World& ActiveWorld() noexcept
	{
		return *ActiveContext().world;
	}
} // namespace aether::app::scripting::interop
