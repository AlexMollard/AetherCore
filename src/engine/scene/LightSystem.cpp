#include "scene/LightSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

#include "rendering/Renderer.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		// Organic flame flicker: intensity dips by up to `flicker` at `speed`, with a per-light phase so
		// neighbouring torches don't pulse in lockstep. Returns a multiplier in [1 - flicker, 1].
		float FlickerMultiplier(float flicker, float speed, float time, std::uint32_t seed)
		{
			if (flicker <= 0.0f)
			{
				return 1.0f;
			}
			const float s = static_cast<float>(seed & 0xFFFFu) * 0.001f;
			const float t = time * speed;
			float n = 0.5f + 0.35f * std::sin(t * 0.7f + s * 6.3f) + 0.25f * std::sin(t * 1.73f + s * 11.1f + 1.1f);
			n = std::clamp(n, 0.0f, 1.0f);
			return 1.0f - flicker * (1.0f - n); // dip toward (1 - flicker) at the troughs
		}
	} // namespace

	void LightSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		m_flickerTime += dt;
		auto& reg = world.GetRegistry();

		auto& points = m_pointLightScratch;
		points.clear();
		points.reserve(reg.view<PointLightComponent, TransformComponent>().size_hint());
		for (const auto e: reg.view<PointLightComponent, TransformComponent>())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(e)))
			{
				continue;
			}
			points.push_back(e);
		}
		std::ranges::sort(points);

		auto& spots = m_spotLightScratch;
		spots.clear();
		spots.reserve(reg.view<SpotLightComponent, TransformComponent>().size_hint());
		for (const auto e: reg.view<SpotLightComponent, TransformComponent>())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(e)))
			{
				continue;
			}
			spots.push_back(e);
		}
		std::ranges::sort(spots);

		m_renderer.ClearPointLights();
		for (const auto e: points)
		{
			const auto& tc = reg.get<TransformComponent>(e);
			const auto& l = reg.get<PointLightComponent>(e);
			const float flick = FlickerMultiplier(l.flicker, l.flickerSpeed, m_flickerTime, static_cast<std::uint32_t>(e));
			m_renderer.AddPointLight(Renderer::PointLight{
			        .position = glm::vec3(tc.localToWorld[3]),
			        .radius = l.radius,
			        .color = l.color,
			        .intensity = l.intensity * flick,
			        .sourceRadius = l.sourceRadius,
			        .castsShadow = l.castsShadow,
			});
		}

		m_renderer.ClearSpotLights();
		for (const auto e: spots)
		{
			const auto& tc = reg.get<TransformComponent>(e);
			const auto& l = reg.get<SpotLightComponent>(e);
			const glm::vec3 forward = -glm::normalize(glm::vec3(tc.localToWorld[2]));
			const float flick = FlickerMultiplier(l.flicker, l.flickerSpeed, m_flickerTime, static_cast<std::uint32_t>(e));
			m_renderer.AddSpotLight(Renderer::SpotLight{
			        .position = glm::vec3(tc.localToWorld[3]),
			        .radius = l.radius,
			        .direction = forward,
			        .innerAngleRad = l.innerAngleRad,
			        .color = l.color,
			        .intensity = l.intensity * flick,
			        .outerAngleRad = l.outerAngleRad,
			        .sourceRadius = l.sourceRadius,
			        .castsShadow = l.castsShadow,
			});
		}
	}
} // namespace aether
