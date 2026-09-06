#include "scripting/interop/InteropCommon.hpp"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "platform/Input.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API std::uint32_t aether_camera_create_orbit(Vec3 pos, Vec3 target, float fovDeg)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto& world = ActiveWorld();
	const glm::vec3 t = ToGlm(target);
	const float distance = glm::length(ToGlm(pos) - t);
	aether::CameraComponent cam{};
	cam.fovDegrees = fovDeg;
	return aether::ecs::CreateOrbitCameraEntity(world, t, 0.0f, 20.0f, distance, cam).id;
	});
}

AE_SCRIPT_API std::uint32_t aether_camera_create_free(Vec3 pos, float fovDeg)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto& world = ActiveWorld();
	aether::CameraComponent cam{};
	cam.fovDegrees = fovDeg;
	return aether::ecs::CreateCameraEntity(world, ToGlm(pos), glm::vec3(0.0f, 0.0f, -1.0f), cam).id;
	});
}

AE_SCRIPT_API std::uint32_t aether_camera_create_orthographic(Vec3 pos, float height)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto& world = ActiveWorld();
	aether::CameraComponent cam{};
	cam.projection = aether::CameraProjection::Orthographic;
	cam.orthographicHeight = glm::max(0.001f, height);
	return aether::ecs::CreateCameraEntity(world, ToGlm(pos), glm::vec3(0.0f, 0.0f, -1.0f), cam).id;
	});
}

AE_SCRIPT_API void aether_camera_set_main(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	// SetMainCameraEntity emplaces MainCameraComponent on the target.
	if (!EntityAlive(id))
	{
		return;
	}
	aether::ecs::SetMainCameraEntity(ActiveWorld(), aether::Entity{id});
	});
}

AE_SCRIPT_API std::uint32_t aether_camera_get_main()
{ return SafeExport([&] -> std::uint32_t { return aether::ecs::GetMainCameraEntity(ActiveWorld()).id; }); }

AE_SCRIPT_API void aether_camera_set_mode(std::uint32_t id, std::int32_t mode)
{
	SafeExport([&] -> void
	{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	auto& reg = world.GetRegistry();
	const auto enttE = aether::World::ToEntt(e);
	if (!reg.valid(enttE))
	{
		return;
	}

	constexpr std::int32_t kOrbit = 0;
	if (mode == kOrbit)
	{
		if (world.TryGet<aether::OrbitCameraComponent>(e) == nullptr)
		{
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
	});
}

AE_SCRIPT_API void aether_camera_set_perspective(std::uint32_t id, float fovDeg)
{
	SafeExport([&] -> void
	{
	if (auto* camera = ActiveWorld().TryGet<aether::CameraComponent>(aether::Entity{id}))
	{
		camera->projection = aether::CameraProjection::Perspective;
		camera->fovDegrees = glm::clamp(fovDeg, 1.0f, 179.0f);
	}
	});
}

AE_SCRIPT_API void aether_camera_set_orthographic(std::uint32_t id, float height)
{
	SafeExport([&] -> void
	{
	if (auto* camera = ActiveWorld().TryGet<aether::CameraComponent>(aether::Entity{id}))
	{
		camera->projection = aether::CameraProjection::Orthographic;
		camera->orthographicHeight = glm::max(0.001f, height);
	}
	});
}

AE_SCRIPT_API std::int32_t aether_camera_get_projection(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	if (const auto* camera = ActiveWorld().TryGet<aether::CameraComponent>(aether::Entity{id}))
	{
		return camera->projection == aether::CameraProjection::Orthographic ? 1 : 0;
	}
	return 0;
	});
}

AE_SCRIPT_API float aether_camera_get_orthographic_height(std::uint32_t id)
{
	return SafeExport([&] -> float
	{
	if (const auto* camera = ActiveWorld().TryGet<aether::CameraComponent>(aether::Entity{id}))
	{
		return camera->orthographicHeight;
	}
	return 0.0f;
	});
}

AE_SCRIPT_API void aether_camera_set_position(std::uint32_t id, Vec3 pos)
{
	SafeExport([&] -> void
	{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (auto* orbit = world.TryGet<aether::OrbitCameraComponent>(e))
	{
		// target: keeps the target lock and re-derives yaw / pitch / distance.
		const glm::vec3 offset = ToGlm(pos) - orbit->target;
		orbit->distance = glm::max(0.1f, glm::length(offset));
		orbit->pitch = glm::degrees(std::asin(glm::clamp(offset.y / orbit->distance, -1.0f, 1.0f)));
		orbit->yaw = glm::degrees(std::atan2(offset.x, offset.z));
		return;
	}
	if (auto* tc = world.TryGet<aether::TransformComponent>(e))
	{
		tc->localToWorld[3] = glm::vec4(ToGlm(pos), 1.0f);
	}
	});
}

AE_SCRIPT_API void aether_camera_set_yaw_pitch(std::uint32_t id, float yaw, float pitch)
{
	SafeExport([&] -> void
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
		const float yr = glm::radians(yaw);
		const float pr = glm::radians(pitch);
		const glm::vec3 forward{
		        -std::sin(yr) * std::cos(pr),
		        std::sin(pr),
		        -std::cos(yr) * std::cos(pr),
		};
		tc->localToWorld = aether::ecs::CameraAimMatrix(glm::vec3(tc->localToWorld[3]), forward);
	}
	});
}

AE_SCRIPT_API void aether_camera_set_target(std::uint32_t id, Vec3 target)
{
	SafeExport([&] -> void
	{
	if (auto* orbit = ActiveWorld().TryGet<aether::OrbitCameraComponent>(aether::Entity{id}))
	{
		orbit->target = ToGlm(target);
	}
	});
}

AE_SCRIPT_API void aether_camera_set_orbital(std::uint32_t id, float yaw, float pitch, float dist)
{
	SafeExport([&] -> void
	{
	if (auto* orbit = ActiveWorld().TryGet<aether::OrbitCameraComponent>(aether::Entity{id}))
	{
		orbit->yaw = yaw;
		orbit->pitch = pitch;
		orbit->distance = dist;
	}
	});
}

AE_SCRIPT_API float aether_camera_get_yaw(std::uint32_t id)
{
	return SafeExport([&] -> float
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
	});
}

AE_SCRIPT_API Vec3 aether_camera_get_forward(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (const auto* orbit = world.TryGet<aether::OrbitCameraComponent>(e))
	{
		// Compute from live orbit params so camera-relative movement never lags a
		const glm::mat4 pose = aether::ecs::OrbitCameraMatrix(orbit->target, orbit->yaw, orbit->pitch, orbit->distance);
		return FromGlm(ForwardOf(pose));
	}
	if (const auto* tc = world.TryGet<aether::TransformComponent>(e))
	{
		return FromGlm(ForwardOf(tc->localToWorld));
	}
	return Vec3{0.0f, 0.0f, -1.0f};
	});
}

AE_SCRIPT_API Vec3 aether_camera_get_right(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
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
	});
}

// Unproject a cursor position (as returned by aether_input_mouse_pos) to a world
// point on the z = 0 plane, using the current main camera. Exact for orthographic
// 2D cameras (the CoinDash case); for perspective it returns the point on the
// camera plane in the cursor direction, which is a reasonable approximation.
AE_SCRIPT_API Vec3 aether_camera_screen_to_world(Vec2 screenPos)
{
	return SafeExport([&] -> Vec3
	{
	auto& ctx = ActiveContext();
	auto& world = ActiveWorld();
	const aether::Entity cam = aether::ecs::GetMainCameraEntity(world);
	const auto* cc = world.TryGet<aether::CameraComponent>(cam);
	const auto* tc = world.TryGet<aether::TransformComponent>(cam);
	if (cc == nullptr || tc == nullptr || ctx.input == nullptr)
	{
		return Vec3{0.0f, 0.0f, 0.0f};
	}

	const glm::vec2 target = ctx.input->GetMouseTargetSize();
	if (target.x <= 0.0f || target.y <= 0.0f)
	{
		return Vec3{0.0f, 0.0f, 0.0f};
	}

	const float aspect = target.x / target.y;
	// Normalized device coords in [-1, 1]; screen Y grows down, world Y grows up.
	const float ndcX = screenPos.x / target.x * 2.0f - 1.0f;
	const float ndcY = 1.0f - screenPos.y / target.y * 2.0f;
	const float halfH = cc->orthographicHeight * 0.5f;
	const float halfW = halfH * aspect;

	const glm::vec3 camPos = glm::vec3(tc->localToWorld[3]);
	const glm::vec3 right = glm::normalize(glm::vec3(tc->localToWorld[0]));
	const glm::vec3 up = glm::normalize(glm::vec3(tc->localToWorld[1]));
	const glm::vec3 pointOnPlane = camPos + right * (ndcX * halfW) + up * (ndcY * halfH);
	return Vec3{pointOnPlane.x, pointOnPlane.y, 0.0f};
	});
}

AE_SCRIPT_API Vec2 aether_camera_world_to_screen(Vec3 worldPos)
{
	return SafeExport([&] -> Vec2
	{
	// Inverse of aether_camera_screen_to_world. Returns render-target pixels with a
	// top-left origin, matching Input.MousePosition and the UI's coordinate space,
	// so a caller can hand the result straight to Ui.SetRect.
	// Behind the camera, returns (-1, -1) so callers can cheaply cull.
	auto& ctx = ActiveContext();
	auto& world = ActiveWorld();
	const aether::Entity cam = aether::ecs::GetMainCameraEntity(world);
	const auto* cc = world.TryGet<aether::CameraComponent>(cam);
	const auto* tc = world.TryGet<aether::TransformComponent>(cam);
	if (cc == nullptr || tc == nullptr || ctx.input == nullptr)
	{
		return Vec2{-1.0f, -1.0f};
	}

	const glm::vec2 target = ctx.input->GetMouseTargetSize();
	if (target.x <= 0.0f || target.y <= 0.0f)
	{
		return Vec2{-1.0f, -1.0f};
	}

	// Mirrors Camera::GetProjectionMatrix, including the [1][1] flip that makes NDC
	// Y grow DOWNWARD under Vulkan - which is why the pixel mapping below is a plain
	// (ndc * 0.5 + 0.5) on both axes and not a flip on one of them.
	const float aspect = target.x / target.y;
	glm::mat4 proj;
	if (cc->projection == aether::CameraProjection::Orthographic)
	{
		const float height = glm::max(0.001f, cc->orthographicHeight);
		const float width = height * glm::max(0.001f, aspect);
		proj = glm::ortho(-width * 0.5f, width * 0.5f, -height * 0.5f, height * 0.5f, cc->nearPlane, cc->farPlane);
	}
	else
	{
		proj = glm::perspective(glm::radians(cc->fovDegrees), aspect, cc->nearPlane, cc->farPlane);
	}
	proj[1][1] *= -1.0f;

	const glm::vec4 clip = proj * glm::inverse(tc->localToWorld) * glm::vec4(ToGlm(worldPos), 1.0f);
	// An orthographic projection always yields w = 1, so this culls only the
	// perspective case - which is correct: screen_to_world maps an orthographic
	// cursor onto the camera plane regardless of depth, and its inverse must agree.
	if (clip.w <= 0.0f)
	{
		return Vec2{-1.0f, -1.0f};
	}
	const glm::vec3 ndc = glm::vec3(clip) / clip.w;
	return Vec2{(ndc.x * 0.5f + 0.5f) * target.x, (ndc.y * 0.5f + 0.5f) * target.y};
	});
}
