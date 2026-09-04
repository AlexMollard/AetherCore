#include "scripting/interop/InteropCommon.hpp"

#include <vector>

#include "rendering/Renderer.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "systems/DayNightSystem.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API void aether_render_set_ambient(Vec3 color)
{ SafeExport([&] -> void { ActiveContext().renderer->SetAmbientLight(ToGlm(color)); }); }

AE_SCRIPT_API void aether_render_set_sun(Vec3 dir, float intensity, Vec3 color)
{
	SafeExport([&] -> void
	{
	auto& r = *ActiveContext().renderer;
	r.SetDirectionalLight(ToGlm(dir), intensity);
	r.SetSunColor(ToGlm(color));
	});
}

AE_SCRIPT_API void aether_render_add_point_light(Vec3 pos, Vec3 color, float intensity, float radius, std::int32_t castsShadow)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	const aether::Entity e = aether::ecs::CreatePointLightEntity(*ctx.world, ToGlm(pos), aether::PointLightComponent{.color = ToGlm(color), .intensity = intensity, .radius = radius, .castsShadow = castsShadow != 0});
	ctx.sceneEntities.push_back(e);
	});
}

AE_SCRIPT_API void aether_render_add_spot_light(Vec3 pos, Vec3 color, float intensity, float radius, Vec3 dir, float innerAngle, float outerAngle, std::int32_t castsShadow)
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	const aether::Entity e = aether::ecs::CreateSpotLightEntity(
	        *ctx.world, ToGlm(pos), ToGlm(dir), aether::SpotLightComponent{.color = ToGlm(color), .intensity = intensity, .radius = radius, .innerAngleRad = innerAngle, .outerAngleRad = outerAngle, .castsShadow = castsShadow != 0});
	ctx.sceneEntities.push_back(e);
	});
}

AE_SCRIPT_API void aether_render_set_point_light_position(std::int32_t idx, Vec3 pos)
{ SafeExport([&] -> void { ActiveContext().renderer->SetPointLightPosition(static_cast<std::uint32_t>(idx), ToGlm(pos)); }); }

AE_SCRIPT_API void aether_render_set_point_light_color(std::int32_t idx, Vec3 color)
{ SafeExport([&] -> void { ActiveContext().renderer->SetPointLightColor(static_cast<std::uint32_t>(idx), ToGlm(color)); }); }

AE_SCRIPT_API void aether_render_set_point_light_intensity(std::int32_t idx, float intensity)
{ SafeExport([&] -> void { ActiveContext().renderer->SetPointLightIntensity(static_cast<std::uint32_t>(idx), intensity); }); }

AE_SCRIPT_API std::int32_t aether_render_get_point_light_count()
{ return SafeExport([&] -> std::int32_t { return static_cast<std::int32_t>(ActiveContext().renderer->GetPointLights().size()); }); }

AE_SCRIPT_API void aether_render_set_spot_light_position(std::int32_t idx, Vec3 pos)
{ SafeExport([&] -> void { ActiveContext().renderer->SetSpotLightPosition(static_cast<std::uint32_t>(idx), ToGlm(pos)); }); }

AE_SCRIPT_API void aether_render_set_spot_light_color(std::int32_t idx, Vec3 color)
{ SafeExport([&] -> void { ActiveContext().renderer->SetSpotLightColor(static_cast<std::uint32_t>(idx), ToGlm(color)); }); }

AE_SCRIPT_API void aether_render_set_spot_light_intensity(std::int32_t idx, float intensity)
{ SafeExport([&] -> void { ActiveContext().renderer->SetSpotLightIntensity(static_cast<std::uint32_t>(idx), intensity); }); }

AE_SCRIPT_API std::int32_t aether_render_get_spot_light_count()
{ return SafeExport([&] -> std::int32_t { return static_cast<std::int32_t>(ActiveContext().renderer->GetSpotLights().size()); }); }

AE_SCRIPT_API void aether_render_clear_lights()
{
	SafeExport([&] -> void
	{
	auto& ctx = ActiveContext();
	auto& world = *ctx.world;
	auto& reg = world.GetRegistry();
	std::vector<aether::Entity> doomed;
	for (const auto e: reg.view<aether::PointLightComponent>())
	{
		doomed.push_back(aether::World::FromEntt(e));
	}
	for (const auto e: reg.view<aether::SpotLightComponent>())
	{
		doomed.push_back(aether::World::FromEntt(e));
	}
	for (const aether::Entity e: doomed)
	{
		std::erase(ctx.sceneEntities, e);
		world.Destroy(e);
	}
	});
}

AE_SCRIPT_API void aether_render_set_sky(Vec3 horizon, Vec3 zenith)
{ SafeExport([&] -> void { ActiveContext().renderer->SetSkyGradient(ToGlm(horizon), ToGlm(zenith)); }); }

AE_SCRIPT_API void aether_render_set_sky_void(Vec3 color)
{ SafeExport([&] -> void { ActiveContext().renderer->SetSkyVoidColor(ToGlm(color)); }); }

AE_SCRIPT_API void aether_daynight_set_enabled(std::int32_t enabled)
{
	SafeExport([&] -> void
	{
	if (auto* dn = ActiveContext().dayNight)
	{
		dn->SetEnabled(enabled != 0);
	}
	});
}

AE_SCRIPT_API std::int32_t aether_daynight_get_enabled()
{
	return SafeExport([&] -> std::int32_t
	{
	auto* dn = ActiveContext().dayNight;
	return dn != nullptr && dn->IsEnabled() ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_daynight_set_time(float hours)
{
	SafeExport([&] -> void
	{
	if (auto* dn = ActiveContext().dayNight)
	{
		dn->SetTimeOfDay(hours);
	}
	});
}

AE_SCRIPT_API float aether_daynight_get_time()
{
	return SafeExport([&] -> float
	{
	auto* dn = ActiveContext().dayNight;
	return dn != nullptr ? dn->GetTimeOfDay() : 12.0f;
	});
}

AE_SCRIPT_API void aether_daynight_set_speed(float secondsPerSecond)
{
	SafeExport([&] -> void
	{
	if (auto* dn = ActiveContext().dayNight)
	{
		dn->SetTimeSpeed(secondsPerSecond);
	}
	});
}

AE_SCRIPT_API float aether_daynight_get_speed()
{
	return SafeExport([&] -> float
	{
	auto* dn = ActiveContext().dayNight;
	return dn != nullptr ? dn->GetTimeSpeed() : 0.0f;
	});
}

AE_SCRIPT_API Vec3 aether_daynight_get_sun_direction()
{
	return SafeExport([&] -> Vec3
	{
	auto* dn = ActiveContext().dayNight;
	return dn != nullptr ? FromGlm(dn->GetSunDirection()) : Vec3{0.0f, 1.0f, 0.0f};
	});
}
