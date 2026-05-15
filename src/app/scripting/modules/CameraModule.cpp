#include "CameraModule.hpp"
#include "DasHelpers.hpp"

#include "daScript/daScript.h"

#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"

namespace
{
	using namespace aether::app::scripting;

	// create_orbit_camera(pos, target, fov_deg) -> uint (camera handle id)
	DasCameraHandle das_create_orbit_camera(das::float3 pos, das::float3 target, float fovDeg)
	{
		auto& ctx = ActiveContext();
		aether::CameraDesc desc{};
		desc.mode = aether::CameraMode::Orbit;
		desc.fovDegrees = fovDeg;
		desc.orbitTarget = { target.x, target.y, target.z };
		desc.orbitYaw = 0.0f;
		desc.orbitPitch = 20.0f;
		desc.orbitDistance = glm::length(glm::vec3{ pos.x - target.x, pos.y - target.y, pos.z - target.z });

		aether::CameraHandle cam = ctx.cameras->Create(desc);
		return cam.id;
	}

	// create_free_camera(pos, fov_deg) -> uint
	DasCameraHandle das_create_free_camera(das::float3 pos, float fovDeg)
	{
		auto& ctx = ActiveContext();
		aether::CameraDesc desc{};
		desc.mode = aether::CameraMode::Free;
		desc.fovDegrees = fovDeg;
		desc.position = { pos.x, pos.y, pos.z };

		aether::CameraHandle cam = ctx.cameras->Create(desc);
		return cam.id;
	}

	void das_set_main_camera(DasCameraHandle id)
	{
		ActiveContext().cameras->SetMainCamera(aether::CameraHandle{ id });
	}

} // namespace

namespace aether::app::scripting
{
	struct CameraModule : das::Module
	{
		CameraModule()
		      : das::Module("camera")
		{
			das::ModuleLibrary lib(this);

			addExtern<DAS_BIND_FUN(das_create_orbit_camera)>(*this, lib, "create_orbit_camera", das::SideEffects::modifyExternal, "das_create_orbit_camera");
			addExtern<DAS_BIND_FUN(das_create_free_camera)>(*this, lib, "create_free_camera", das::SideEffects::modifyExternal, "das_create_free_camera");
			addExtern<DAS_BIND_FUN(das_set_main_camera)>(*this, lib, "set_main_camera", das::SideEffects::modifyExternal, "das_set_main_camera");

			verifyAotReady();
		}
	};

} // namespace aether::app::scripting

REGISTER_MODULE_IN_NAMESPACE(CameraModule, aether::app::scripting);

void RegisterCameraModule()
{
	NEED_MODULE(CameraModule);
}
