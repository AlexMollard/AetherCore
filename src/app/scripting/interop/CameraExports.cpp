#include "scripting/interop/InteropCommon.hpp"

#include <cmath>

#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

// Camera control exported to C#. A camera handle (managed CameraId) is the id of
// the ENTITY that carries the CameraComponent, so every script camera is a
// first-class node in the scene hierarchy. Orbit cameras additionally carry an
// OrbitCameraComponent that CameraSystem uses to drive their pose each frame.
// There is no longer any entity-less camera on the scripting side: the
// CameraManager pool is an engine-internal backing store CameraSystem mirrors
// into, so "you cannot have a scene camera that isn't an entity" holds by
// construction.

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	// Camera-forward from a world matrix: local -Z, normalized. Matches
	// CameraSystem::ForwardOf and the retired Camera::GetForward convention.
	glm::vec3 ForwardOf(const glm::mat4& m)
	{
		const glm::vec3 fwd = -glm::vec3(m[2]);
		const float len = glm::length(fwd);
		return len > 1e-6f ? fwd / len : glm::vec3(0.0f, 0.0f, -1.0f);
	}
} // namespace

AE_SCRIPT_API std::uint32_t aether_camera_create_orbit(Vec3 pos, Vec3 target, float fovDeg)
{
	auto& world = ActiveWorld();
	const glm::vec3 t = ToGlm(target);
	const float distance = glm::length(ToGlm(pos) - t);
	aether::CameraComponent cam{};
	cam.fovDegrees = fovDeg;
	// Seed framing the way the old raw orbit camera did: yaw 0 (camera on +Z of
	// the target), pitch 20 degrees up. The passed-in `pos` only sets distance.
	return aether::ecs::CreateOrbitCameraEntity(world, t, 0.0f, 20.0f, distance, cam).id;
}

AE_SCRIPT_API std::uint32_t aether_camera_create_free(Vec3 pos, float fovDeg)
{
	auto& world = ActiveWorld();
	aether::CameraComponent cam{};
	cam.fovDegrees = fovDeg;
	// No OrbitCameraComponent: a free camera's pose is whatever the script sets
	// through SetPosition / SetYawPitch. Start at `pos` looking down -Z.
	return aether::ecs::CreateCameraEntity(world, ToGlm(pos), glm::vec3(0.0f, 0.0f, -1.0f), cam).id;
}

AE_SCRIPT_API void aether_camera_set_main(std::uint32_t id)
{
	aether::ecs::SetMainCameraEntity(ActiveWorld(), aether::Entity{id});
}

// The scene's current main-camera entity (0 if none tagged).
AE_SCRIPT_API std::uint32_t aether_camera_get_main()
{
	return aether::ecs::GetMainCameraEntity(ActiveWorld()).id;
}

AE_SCRIPT_API void aether_camera_set_mode(std::uint32_t id, std::int32_t mode)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	auto& reg = world.GetRegistry();
	const auto enttE = aether::World::ToEntt(e);
	if (!reg.valid(enttE))
	{
		return;
	}

	// Managed CameraMode: Orbit = 0, Free = 1. Switching modes adds or removes the
	// OrbitCameraComponent; CameraSystem drives the pose only while it is present.
	constexpr std::int32_t kOrbit = 0;
	if (mode == kOrbit)
	{
		if (world.TryGet<aether::OrbitCameraComponent>(e) == nullptr)
		{
			// Seed orbit params from the current pose so the switch doesn't jump:
			// orbit a target one default distance ahead of the current forward.
			const auto* tc = world.TryGet<aether::TransformComponent>(e);
			const glm::vec3 eye = tc != nullptr ? glm::vec3(tc->localToWorld[3]) : glm::vec3(0.0f);
			const glm::vec3 fwd = tc != nullptr ? ForwardOf(tc->localToWorld) : glm::vec3(0.0f, 0.0f, -1.0f);
			constexpr float kDist = 10.0f;
			aether::OrbitCameraComponent orbit{};
			orbit.target = eye + fwd * kDist;
			orbit.distance = kDist;
			orbit.pitch = glm::degrees(std::asin(glm::clamp(-fwd.y, -1.0f, 1.0f)));
			orbit.yaw = glm::degrees(std::atan2(-fwd.x, -fwd.z));
			world.Emplace<aether::OrbitCameraComponent>(e, orbit);
		}
	}
	else if (reg.all_of<aether::OrbitCameraComponent>(enttE))
	{
		reg.remove<aether::OrbitCameraComponent>(enttE);
	}
}

AE_SCRIPT_API void aether_camera_set_position(std::uint32_t id, Vec3 pos)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (auto* orbit = world.TryGet<aether::OrbitCameraComponent>(e))
	{
		// Reinterpret the requested eye as an orbit position around the same
		// target: keeps the target lock and re-derives yaw / pitch / distance.
		const glm::vec3 offset = ToGlm(pos) - orbit->target;
		orbit->distance = glm::max(0.1f, glm::length(offset));
		orbit->pitch = glm::degrees(std::asin(glm::clamp(offset.y / orbit->distance, -1.0f, 1.0f)));
		orbit->yaw = glm::degrees(std::atan2(offset.x, offset.z));
		return;
	}
	if (auto* tc = world.TryGet<aether::TransformComponent>(e))
	{
		// Free camera: translate, keep orientation.
		tc->localToWorld[3] = glm::vec4(ToGlm(pos), 1.0f);
	}
}

AE_SCRIPT_API void aether_camera_set_yaw_pitch(std::uint32_t id, float yaw, float pitch)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (auto* orbit = world.TryGet<aether::OrbitCameraComponent>(e))
	{
		orbit->yaw = yaw;
		orbit->pitch = pitch;
		return;
	}
	if (auto* tc = world.TryGet<aether::TransformComponent>(e))
	{
		// Free camera: yaw / pitch set the look direction (yaw=0, pitch=0 -> -Z)
		// about the current eye. Inverse of CameraSystem's forward->yaw/pitch.
		const float yr = glm::radians(yaw);
		const float pr = glm::radians(pitch);
		const glm::vec3 forward{
		        -std::sin(yr) * std::cos(pr),
		        std::sin(pr),
		        -std::cos(yr) * std::cos(pr),
		};
		tc->localToWorld = aether::ecs::CameraAimMatrix(glm::vec3(tc->localToWorld[3]), forward);
	}
}

AE_SCRIPT_API void aether_camera_set_target(std::uint32_t id, Vec3 target)
{
	if (auto* orbit = ActiveWorld().TryGet<aether::OrbitCameraComponent>(aether::Entity{id}))
	{
		orbit->target = ToGlm(target);
	}
}

AE_SCRIPT_API void aether_camera_set_orbital(std::uint32_t id, float yaw, float pitch, float dist)
{
	if (auto* orbit = ActiveWorld().TryGet<aether::OrbitCameraComponent>(aether::Entity{id}))
	{
		orbit->yaw = yaw;
		orbit->pitch = pitch;
		orbit->distance = dist;
	}
}

AE_SCRIPT_API float aether_camera_get_yaw(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (const auto* orbit = world.TryGet<aether::OrbitCameraComponent>(e))
	{
		return orbit->yaw;
	}
	if (const auto* tc = world.TryGet<aether::TransformComponent>(e))
	{
		const glm::vec3 fwd = ForwardOf(tc->localToWorld);
		return glm::degrees(std::atan2(-fwd.x, -fwd.z));
	}
	return 0.0f;
}

AE_SCRIPT_API Vec3 aether_camera_get_forward(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (const auto* orbit = world.TryGet<aether::OrbitCameraComponent>(e))
	{
		// Compute from live orbit params so camera-relative movement never lags a
		// frame behind CameraSystem's transform write.
		const glm::mat4 pose = aether::ecs::OrbitCameraMatrix(orbit->target, orbit->yaw, orbit->pitch, orbit->distance);
		return FromGlm(ForwardOf(pose));
	}
	if (const auto* tc = world.TryGet<aether::TransformComponent>(e))
	{
		return FromGlm(ForwardOf(tc->localToWorld));
	}
	return Vec3{0.0f, 0.0f, -1.0f};
}

AE_SCRIPT_API Vec3 aether_camera_get_right(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	glm::mat4 pose(1.0f);
	if (const auto* orbit = world.TryGet<aether::OrbitCameraComponent>(e))
	{
		pose = aether::ecs::OrbitCameraMatrix(orbit->target, orbit->yaw, orbit->pitch, orbit->distance);
	}
	else if (const auto* tc = world.TryGet<aether::TransformComponent>(e))
	{
		pose = tc->localToWorld;
	}
	else
	{
		return Vec3{1.0f, 0.0f, 0.0f};
	}
	const glm::vec3 right = glm::vec3(pose[0]);
	const float len = glm::length(right);
	return FromGlm(len > 1e-6f ? right / len : glm::vec3(1.0f, 0.0f, 0.0f));
}
