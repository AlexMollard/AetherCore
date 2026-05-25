#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "rendering/Renderer.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/DayNightSystem.hpp"

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

	void das_add_point_light(das::float3 pos, das::float3 color, float intensity, float radius, bool castsShadow)
	{
		auto& r = *ActiveContext().renderer;
		std::vector<aether::Renderer::PointLight> lights(r.GetPointLights().begin(), r.GetPointLights().end());
		lights.push_back({
		        .position = {   pos.x,   pos.y,   pos.z },
		        .radius = radius,
		        .color = { color.x, color.y, color.z },
		        .intensity = intensity,
		        .castsShadow = castsShadow,
		});
		r.SetPointLights(std::move(lights));
	}

	void das_add_spot_light(das::float3 pos, das::float3 color, float intensity, float radius, das::float3 dir, float innerAngle, float outerAngle, bool castsShadow)
	{
		auto& r = *ActiveContext().renderer;
		std::vector<aether::Renderer::SpotLight> lights(r.GetSpotLights().begin(), r.GetSpotLights().end());
		lights.push_back({
		        .position = {   pos.x,   pos.y,   pos.z },
		        .radius = radius,
		        .direction = {  dir.x,   dir.y,   dir.z },
		        .innerAngleRad = innerAngle,
		        .color = { color.x, color.y, color.z },
		        .intensity = intensity,
		        .outerAngleRad = outerAngle,
		        .castsShadow = castsShadow,
		});
		r.SetSpotLights(std::move(lights));
	}

	void das_set_sky(das::float3 horizon, das::float3 zenith)
	{
		ActiveContext().renderer->SetSkyGradient({ horizon.x, horizon.y, horizon.z }, { zenith.x, zenith.y, zenith.z });
	}

	void das_set_sky_void(das::float3 color)
	{
		ActiveContext().renderer->SetSkyVoidColor({ color.x, color.y, color.z });
	}

	// ── Day/Night cycle controls ──────────────────────────────────────────────

	void das_set_day_night_enabled(bool enabled)
	{
		if (auto* dn = ActiveContext().dayNight)
		{
			dn->SetEnabled(enabled);
		}
	}

	bool das_get_day_night_enabled()
	{
		if (auto* dn = ActiveContext().dayNight)
		{
			return dn->IsEnabled();
		}
		return false;
	}

	void das_set_time_of_day(float hours)
	{
		if (auto* dn = ActiveContext().dayNight)
		{
			dn->SetTimeOfDay(hours);
		}
	}

	float das_get_time_of_day()
	{
		if (auto* dn = ActiveContext().dayNight)
		{
			return dn->GetTimeOfDay();
		}
		return 12.0f;
	}

	void das_set_time_speed(float secondsPerSecond)
	{
		if (auto* dn = ActiveContext().dayNight)
		{
			dn->SetTimeSpeed(secondsPerSecond);
		}
	}

	float das_get_time_speed()
	{
		if (auto* dn = ActiveContext().dayNight)
		{
			return dn->GetTimeSpeed();
		}
		return 0.0f;
	}

	das::float3 das_get_sun_direction()
	{
		if (auto* dn = ActiveContext().dayNight)
		{
			const auto d = dn->GetSunDirection();
			return { d.x, d.y, d.z };
		}
		return { 0.0f, 1.0f, 0.0f };
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
			Bind<das_add_spot_light>(lib, "add_spot_light", SE::modifyExternal);
			Bind<das_set_sky>(lib, "set_sky", SE::modifyExternal);
			Bind<das_set_sky_void>(lib, "set_sky_void", SE::modifyExternal);

			Bind<das_set_day_night_enabled>(lib, "set_day_night_enabled", SE::modifyExternal);
			Bind<das_get_day_night_enabled>(lib, "get_day_night_enabled", SE::accessExternal);
			Bind<das_set_time_of_day>(lib, "set_time_of_day", SE::modifyExternal);
			Bind<das_get_time_of_day>(lib, "get_time_of_day", SE::accessExternal);
			Bind<das_set_time_speed>(lib, "set_time_speed", SE::modifyExternal);
			Bind<das_get_time_speed>(lib, "get_time_speed", SE::accessExternal);
			Bind<das_get_sun_direction>(lib, "get_sun_direction", SE::accessExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(RendererModule, aether::app::scripting)
