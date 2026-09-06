#pragma once

#if defined(__clang__)
#	pragma clang diagnostic ignored "-Wreturn-type-c-linkage" // POD value returns are part of the fixed managed ABI.
#endif

#include <algorithm>
#include <cstdint>
#include <exception>
#include <type_traits>

#include <glm/glm.hpp>

#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

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

	// World-space forward from a local-to-world matrix: -Z, normalized (this engine's
	// forward convention - matches glTF/OpenGL). Shared by CameraExports.cpp's
	// aether_camera_get_forward and WorldExports.cpp's generic aether_get_forward, so a
	// non-camera entity's "forward" (a turret, an emitter) is never a second,
	// independently-derived definition that can silently disagree with the camera's -
	// exactly the failure mode a hand-rolled yaw/pitch trig forward already caused once
	// in this codebase (see FirstPersonPlayer.cs's own file history).
	[[nodiscard]] inline glm::vec3 ForwardOf(const glm::mat4& m) noexcept
	{
		const glm::vec3 fwd = -glm::vec3(m[2]);
		const float len = glm::length(fwd);
		return len > 1e-6f ? fwd / len : glm::vec3(0.0f, 0.0f, -1.0f);
	}

	[[nodiscard]] inline aether::World& ActiveWorld() noexcept
	{
		return *ActiveContext().world;
	}

	// Managed code hands exports raw uint32 entity ids it can fabricate or hold past
	// a destroy. entt attaches components to such ids without complaint (growing
	// sparse sets, leaving the components to silently ride along on the next recycled
	// id), so every export that emplaces a component - or calls an engine helper that
	// does - checks aliveness first and quietly refuses dead ids, matching the
	// TryGet-based getters that already return their failure sentinel.
	[[nodiscard]] inline bool EntityAlive(std::uint32_t id) noexcept
	{
		return ActiveWorld().GetRegistry().valid(aether::World::ToEntt(aether::Entity{id}));
	}

	// Entity.Destroy is deferred to the end of the script update, so an entity can be
	// registry-valid for the rest of the frame while already booked for destruction.
	// Validity as a script should observe it (Entity.Valid) excludes those.
	[[nodiscard]] inline bool IsPendingDestroy(std::uint32_t id) noexcept
	{
		const auto& pending = ActiveContext().pendingDestroys;
		return std::find(pending.begin(), pending.end(), aether::Entity{id}) != pending.end();
	}

	// No C++ exception may unwind out of an AE_SCRIPT_API export into managed frames
	// (LibraryImport / UnmanagedCallersOnly): the CLR cannot unwind them, so what
	// should be a logged script error instead FailFasts the process. Every export
	// body runs through this barrier, which reports the failure and returns the
	// export's zero failure sentinel.
	template<typename Fn>
	auto SafeExport(Fn&& fn) noexcept -> decltype(fn())
	{
		try
		{
			return fn();
		}
		catch (const std::exception& ex)
		{
			AE_ERROR(aether::LogCategory::App, "Script export failed with exception: {}", ex.what());
		}
		catch (...)
		{
			AE_ERROR(aether::LogCategory::App, "Script export failed with unknown exception");
		}
		using Result = decltype(fn());
		if constexpr (!std::is_void_v<Result>)
		{
			return Result{};
		}
	}
} // namespace aether::app::scripting::interop
