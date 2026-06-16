#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "scripting/SceneContext.hpp"

namespace
{
	using namespace aether::app::scripting;

	// create_orbit_camera(pos, target, fov_deg) -> uint (camera handle id)
	uint32_t das_create_orbit_camera(das::float3 pos, das::float3 target, float fovDeg)
	{
		auto& ctx = ActiveContext();
		aether::CameraDesc desc{};
		desc.mode = aether::CameraMode::Orbit;
		desc.fovDegrees = fovDeg;
		desc.orbitTarget = {target.x, target.y, target.z};
		desc.orbitYaw = 0.0f;
		desc.orbitPitch = 20.0f;
		desc.orbitDistance = glm::length(glm::vec3{pos.x - target.x, pos.y - target.y, pos.z - target.z});
		return ctx.cameras->Create(desc).id;
	}

	// create_free_camera(pos, fov_deg) -> uint
	uint32_t das_create_free_camera(das::float3 pos, float fovDeg)
	{
		auto& ctx = ActiveContext();
		aether::CameraDesc desc{};
		desc.mode = aether::CameraMode::Free;
		desc.fovDegrees = fovDeg;
		desc.position = {pos.x, pos.y, pos.z};
		return ctx.cameras->Create(desc).id;
	}

	void das_set_main_camera(uint32_t id)
	{
		ActiveContext().cameras->SetMainCamera(aether::CameraHandle{id});
	}

	void das_set_camera_mode(uint32_t id, int mode)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		if (cam)
		{
			cam->SetMode(static_cast<aether::CameraMode>(mode));
		}
	}

	void das_set_camera_position(uint32_t id, float x, float y, float z)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		if (cam)
		{
			cam->SetPosition({x, y, z});
		}
	}

	void das_set_camera_yaw_pitch(uint32_t id, float yaw, float pitch)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		if (cam)
		{
			cam->SetYawPitch(yaw, pitch);
		}
	}

	void das_set_camera_target(uint32_t id, float x, float y, float z)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		if (cam)
		{
			cam->SetOrbitTarget({x, y, z});
		}
	}

	void das_set_camera_orbital(uint32_t id, float yaw, float pitch, float dist)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		if (cam)
		{
			cam->SetOrbitYawPitch(yaw, pitch);
			cam->SetOrbitDistance(dist);
		}
	}

	float das_get_camera_yaw(uint32_t id)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		return cam ? cam->GetOrbitYaw() : 0.0f;
	}

	// get_camera_forward(id) -> float3
	das::float3 das_get_camera_forward(uint32_t id)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		if (!cam)
		{
			return {0.0f, 0.0f, -1.0f};
		}
		glm::vec3 fwd = cam->GetForward();
		return {fwd.x, fwd.y, fwd.z};
	}

	// get_camera_right(id) -> float3
	das::float3 das_get_camera_right(uint32_t id)
	{
		auto cam = ActiveContext().cameras->TryGet(aether::CameraHandle{id});
		if (!cam)
		{
			return {1.0f, 0.0f, 0.0f};
		}
		glm::vec3 right = cam->GetRight();
		return {right.x, right.y, right.z};
	}

} // namespace

namespace aether::app::scripting
{
	struct CameraModule : DasModuleBase
	{
		CameraModule()
		      : DasModuleBase("camera")
		{
			das::ModuleLibrary lib(this);

			Bind<das_create_orbit_camera>(lib, "create_orbit_camera", SE::modifyExternal);
			Bind<das_create_free_camera>(lib, "create_free_camera", SE::modifyExternal);
			Bind<das_set_main_camera>(lib, "set_main_camera", SE::modifyExternal);
			Bind<das_set_camera_mode>(lib, "set_camera_mode", SE::modifyExternal);
			Bind<das_set_camera_position>(lib, "set_camera_position", SE::modifyExternal);
			Bind<das_set_camera_yaw_pitch>(lib, "set_camera_yaw_pitch", SE::modifyExternal);
			Bind<das_set_camera_target>(lib, "set_camera_target", SE::modifyExternal);
			Bind<das_set_camera_orbital>(lib, "set_camera_orbital", SE::modifyExternal);
			Bind<das_get_camera_yaw>(lib, "get_camera_yaw", SE::accessExternal);
			Bind<das_get_camera_forward>(lib, "get_camera_forward", SE::accessExternal);
			Bind<das_get_camera_right>(lib, "get_camera_right", SE::accessExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(CameraModule, aether::app::scripting)
