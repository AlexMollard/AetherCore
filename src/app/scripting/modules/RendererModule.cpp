#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "rendering/Renderer.hpp"
#include "scripting/DasHelpers.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/DayNightSystem.hpp"

namespace
{
	using namespace aether::app::scripting;

	void das_set_ambient(das::float3 color)
	{
		ActiveContext().renderer->SetAmbientLight(to_glm(color));
	}

	void das_set_sun(das::float3 dir, float intensity, das::float3 color)
	{
		auto& r = *ActiveContext().renderer;
		r.SetDirectionalLight(to_glm(dir), intensity);
		r.SetSunColor(to_glm(color));
	}

	void das_add_point_light(das::float3 pos, das::float3 color, float intensity, float radius, bool castsShadow)
	{
		ActiveContext().renderer->AddPointLight({
		        .position = to_glm(pos),
		        .radius = radius,
		        .color = to_glm(color),
		        .intensity = intensity,
		        .castsShadow = castsShadow,
		});
	}

	void das_add_spot_light(das::float3 pos, das::float3 color, float intensity, float radius, das::float3 dir, float innerAngle, float outerAngle, bool castsShadow)
	{
		ActiveContext().renderer->AddSpotLight({
		        .position = to_glm(pos),
		        .radius = radius,
		        .direction = to_glm(dir),
		        .innerAngleRad = innerAngle,
		        .color = to_glm(color),
		        .intensity = intensity,
		        .outerAngleRad = outerAngle,
		        .castsShadow = castsShadow,
		});
	}

	void das_set_sky(das::float3 horizon, das::float3 zenith)
	{
		ActiveContext().renderer->SetSkyGradient(to_glm(horizon), to_glm(zenith));
	}

	void das_set_point_light_position(int idx, das::float3 pos)
	{
		ActiveContext().renderer->SetPointLightPosition(static_cast<std::uint32_t>(idx), to_glm(pos));
	}

	void das_set_point_light_color(int idx, das::float3 color)
	{
		ActiveContext().renderer->SetPointLightColor(static_cast<std::uint32_t>(idx), to_glm(color));
	}

	void das_set_point_light_intensity(int idx, float intensity)
	{
		ActiveContext().renderer->SetPointLightIntensity(static_cast<std::uint32_t>(idx), intensity);
	}

	int das_get_point_light_count()
	{
		return static_cast<int>(ActiveContext().renderer->GetPointLights().size());
	}

	void das_set_spot_light_position(int idx, das::float3 pos)
	{
		ActiveContext().renderer->SetSpotLightPosition(static_cast<std::uint32_t>(idx), to_glm(pos));
	}

	void das_set_spot_light_color(int idx, das::float3 color)
	{
		ActiveContext().renderer->SetSpotLightColor(static_cast<std::uint32_t>(idx), to_glm(color));
	}

	void das_set_spot_light_intensity(int idx, float intensity)
	{
		ActiveContext().renderer->SetSpotLightIntensity(static_cast<std::uint32_t>(idx), intensity);
	}

	int das_get_spot_light_count()
	{
		return static_cast<int>(ActiveContext().renderer->GetSpotLights().size());
	}

	void das_clear_lights()
	{
		auto& r = *ActiveContext().renderer;
		r.ClearPointLights();
		r.ClearSpotLights();
	}

	void das_set_sky_void(das::float3 color)
	{
		ActiveContext().renderer->SetSkyVoidColor(to_glm(color));
	}

	// -- Day/Night cycle controls ----------------------------------------------

	void das_set_day_night_enabled(bool enabled)
	{
		if (auto dn = ActiveContext().dayNight)
		{
			dn->SetEnabled(enabled);
		}
	}

	bool das_get_day_night_enabled()
	{
		if (auto dn = ActiveContext().dayNight)
		{
			return dn->IsEnabled();
		}
		return false;
	}

	void das_set_time_of_day(float hours)
	{
		if (auto dn = ActiveContext().dayNight)
		{
			dn->SetTimeOfDay(hours);
		}
	}

	float das_get_time_of_day()
	{
		if (auto dn = ActiveContext().dayNight)
		{
			return dn->GetTimeOfDay();
		}
		return 12.0f;
	}

	void das_set_time_speed(float secondsPerSecond)
	{
		if (auto dn = ActiveContext().dayNight)
		{
			dn->SetTimeSpeed(secondsPerSecond);
		}
	}

	float das_get_time_speed()
	{
		if (auto dn = ActiveContext().dayNight)
		{
			return dn->GetTimeSpeed();
		}
		return 0.0f;
	}

	das::float3 das_get_sun_direction()
	{
		if (auto dn = ActiveContext().dayNight)
		{
			return to_das(dn->GetSunDirection());
		}
		return {0.0f, 1.0f, 0.0f};
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
			Bind<das_set_point_light_position>(lib, "set_point_light_position", SE::modifyExternal);
			Bind<das_set_point_light_color>(lib, "set_point_light_color", SE::modifyExternal);
			Bind<das_set_point_light_intensity>(lib, "set_point_light_intensity", SE::modifyExternal);
			Bind<das_get_point_light_count>(lib, "get_point_light_count", SE::accessExternal);
			Bind<das_add_spot_light>(lib, "add_spot_light", SE::modifyExternal);
			Bind<das_set_spot_light_position>(lib, "set_spot_light_position", SE::modifyExternal);
			Bind<das_set_spot_light_color>(lib, "set_spot_light_color", SE::modifyExternal);
			Bind<das_set_spot_light_intensity>(lib, "set_spot_light_intensity", SE::modifyExternal);
			Bind<das_get_spot_light_count>(lib, "get_spot_light_count", SE::accessExternal);
			Bind<das_clear_lights>(lib, "clear_lights", SE::modifyExternal);
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
