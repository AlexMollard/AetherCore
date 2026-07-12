#pragma once

#include <cstdint>

#include <glm/glm.hpp>

#include "scripting/SceneContext.hpp"

// ── C# <-> C++ interop exports ────────────────────────────────────────────────
//
// Engine APIs are exposed to C# as plain C functions exported directly from the
// App executable. C# binds them with [LibraryImport("AetherHost")] and a
// DllImportResolver that maps "AetherHost" to the running module. Each binding is
// therefore just one AE_SCRIPT_API function here plus one partial-method line on
// the managed side - no registration tables, no per-call marshalling.
//
// The ABI is deliberately blittable: entity ids are uint32, vectors are Vec3
// (== System.Numerics.Vector3), strings are UTF-8 byte pointers, and booleans
// cross as int32 (0/1) because LibraryImport requires explicit bool marshalling.
//
// Every export runs inside a managed script call, so scripting::ActiveContext()
// (the TLS SceneContext the runner installs) is valid inside an export body.

#ifdef _WIN32
#	define AE_SCRIPT_API extern "C" __declspec(dllexport)
#else
#	define AE_SCRIPT_API extern "C" __attribute__((visibility("default")))
#endif

namespace aether::app::scripting::interop
{
	// Blittable 3-float vector matching System.Numerics.Vector3 and glm::vec3.
	struct Vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	static_assert(sizeof(Vec3) == 12, "Vec3 must be 3 tightly-packed floats to match Vector3/glm::vec3");
	static_assert(sizeof(glm::vec3) == sizeof(Vec3), "glm::vec3 layout must match interop Vec3");

	// Blittable 2- and 4-float vectors matching System.Numerics.Vector2/Vector4.
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

	// The World bound to the current managed call. The runner installs
	// g_activeContext before every managed invocation, so this is always valid
	// inside an export body.
	[[nodiscard]] inline aether::World& ActiveWorld() noexcept
	{
		return *ActiveContext().world;
	}
} // namespace aether::app::scripting::interop
