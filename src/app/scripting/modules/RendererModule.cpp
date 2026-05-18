#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "rendering/Renderer.hpp"
#include "scripting/SceneContext.hpp"

namespace
{
	using namespace aether::app::scripting;

	void das_set_ambient(das::float3 color)
	{
		ActiveContext().renderer->SetAmbientLight({ color.x, color.y, color.z });
	}

	void das_set_sun(das::float3 dir, float intensity, das::float3 color)
	{
		auto& r = *ActiveContext().renderer;
		r.SetDirectionalLight({ dir.x, dir.y, dir.z }, intensity);
		r.SetSunColor({ color.x, color.y, color.z });
	}

	void das_add_point_light(das::float3 pos, das::float3 color, float intensity, float radius)
	{
		auto& r = *ActiveContext().renderer;
		std::vector<aether::Renderer::PointLight> lights(r.GetPointLights().begin(), r.GetPointLights().end());
		lights.push_back({
		        .position = {   pos.x,   pos.y,   pos.z },
		        .radius = radius,
		        .color = { color.x, color.y, color.z },
		        .intensity = intensity,
		});
		r.SetPointLights(std::move(lights));
	}

	void das_set_sky(das::float3 horizon, das::float3 zenith)
	{
		ActiveContext().renderer->SetSkyGradient({ horizon.x, horizon.y, horizon.z }, { zenith.x, zenith.y, zenith.z });
	}

} // namespace

namespace aether::app::scripting
{
	struct RendererModule : DasModuleBase
	{
		RendererModule()
		      : DasModuleBase("renderer")
		{
			das::ModuleLibrary lib(this);

			Bind<das_set_ambient>(lib, "set_ambient", SE::modifyExternal);
			Bind<das_set_sun>(lib, "set_sun", SE::modifyExternal);
			Bind<das_add_point_light>(lib, "add_point_light", SE::modifyExternal);
			Bind<das_set_sky>(lib, "set_sky", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(RendererModule, aether::app::scripting)
