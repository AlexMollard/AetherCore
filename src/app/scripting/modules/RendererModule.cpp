#include "RendererModule.hpp"
#include "DasHelpers.hpp"

#include "daScript/daScript.h"

#include "rendering/Renderer.hpp"

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
	struct RendererModule : das::Module
	{
		RendererModule()
		      : das::Module("renderer")
		{
			das::ModuleLibrary lib(this);

			addExtern<DAS_BIND_FUN(das_set_ambient)>(*this, lib, "set_ambient", das::SideEffects::modifyExternal, "das_set_ambient");
			addExtern<DAS_BIND_FUN(das_set_sun)>(*this, lib, "set_sun", das::SideEffects::modifyExternal, "das_set_sun");
			addExtern<DAS_BIND_FUN(das_add_point_light)>(*this, lib, "add_point_light", das::SideEffects::modifyExternal, "das_add_point_light");
			addExtern<DAS_BIND_FUN(das_set_sky)>(*this, lib, "set_sky", das::SideEffects::modifyExternal, "das_set_sky");

			verifyAotReady();
		}
	};

} // namespace aether::app::scripting

REGISTER_MODULE_IN_NAMESPACE(RendererModule, aether::app::scripting);

void RegisterRendererModule()
{
	NEED_MODULE(RendererModule);
}
