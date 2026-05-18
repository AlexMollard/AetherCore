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
		desc.orbitTarget = { target.x, target.y, target.z };
		desc.orbitYaw = 0.0f;
		desc.orbitPitch = 20.0f;
		desc.orbitDistance = glm::length(glm::vec3{ pos.x - target.x, pos.y - target.y, pos.z - target.z });
		return ctx.cameras->Create(desc).id;
	}

	// create_free_camera(pos, fov_deg) -> uint
	uint32_t das_create_free_camera(das::float3 pos, float fovDeg)
	{
		auto& ctx = ActiveContext();
		aether::CameraDesc desc{};
		desc.mode = aether::CameraMode::Free;
		desc.fovDegrees = fovDeg;
		desc.position = { pos.x, pos.y, pos.z };
		return ctx.cameras->Create(desc).id;
	}

	void das_set_main_camera(uint32_t id)
	{
		ActiveContext().cameras->SetMainCamera(aether::CameraHandle{ id });
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

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(CameraModule, aether::app::scripting)
