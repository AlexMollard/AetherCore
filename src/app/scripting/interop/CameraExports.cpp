#include "scripting/interop/InteropCommon.hpp"

#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"

// Camera control exported to C#. Camera handles are opaque uint ids (managed
// CameraId). Bodies mirror the old daScript CameraModule.

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API std::uint32_t aether_camera_create_orbit(Vec3 pos, Vec3 target, float fovDeg)
{
	auto& ctx = ActiveContext();
	aether::CameraDesc desc{};
	desc.mode = aether::CameraMode::Orbit;
	desc.fovDegrees = fovDeg;
	desc.orbitTarget = ToGlm(target);
	desc.orbitYaw = 0.0f;
	desc.orbitPitch = 20.0f;
	desc.orbitDistance = glm::length(ToGlm(pos) - ToGlm(target));
	return ctx.cameras->Create(desc).id;
}

AE_SCRIPT_API std::uint32_t aether_camera_create_free(Vec3 pos, float fovDeg)
{
	auto& ctx = ActiveContext();
	aether::CameraDesc desc{};
	desc.mode = aether::CameraMode::Free;
	desc.fovDegrees = fovDeg;
	desc.position = ToGlm(pos);
	return ctx.cameras->Create(desc).id;
}

AE_SCRIPT_API void aether_camera_set_main(std::uint32_t id)
{
	ActiveContext().cameras->SetMainCamera(aether::CameraHandle{id});
}

AE_SCRIPT_API void aether_camera_set_mode(std::uint32_t id, std::int32_t mode)
{
	if (auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id}))
	{
		cam->SetMode(static_cast<aether::CameraMode>(mode));
	}
}

AE_SCRIPT_API void aether_camera_set_position(std::uint32_t id, Vec3 pos)
{
	if (auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id}))
	{
		cam->SetPosition(ToGlm(pos));
	}
}

AE_SCRIPT_API void aether_camera_set_yaw_pitch(std::uint32_t id, float yaw, float pitch)
{
	if (auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id}))
	{
		cam->SetYawPitch(yaw, pitch);
	}
}

AE_SCRIPT_API void aether_camera_set_target(std::uint32_t id, Vec3 target)
{
	if (auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id}))
	{
		cam->SetOrbitTarget(ToGlm(target));
	}
}

AE_SCRIPT_API void aether_camera_set_orbital(std::uint32_t id, float yaw, float pitch, float dist)
{
	if (auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id}))
	{
		cam->SetOrbitYawPitch(yaw, pitch);
		cam->SetOrbitDistance(dist);
	}
}

AE_SCRIPT_API float aether_camera_get_yaw(std::uint32_t id)
{
	auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
	return cam != nullptr ? cam->GetOrbitYaw() : 0.0f;
}

AE_SCRIPT_API Vec3 aether_camera_get_forward(std::uint32_t id)
{
	auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
	return cam != nullptr ? FromGlm(cam->GetForward()) : Vec3{0.0f, 0.0f, -1.0f};
}

AE_SCRIPT_API Vec3 aether_camera_get_right(std::uint32_t id)
{
	auto* cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
	return cam != nullptr ? FromGlm(cam->GetRight()) : Vec3{1.0f, 0.0f, 0.0f};
}
