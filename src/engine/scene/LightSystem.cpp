#include "scene/LightSystem.hpp"

#include <algorithm>

#include <glm/glm.hpp>

#include "rendering/Renderer.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void LightSystem::Update(World& world, float)
	{
		AE_PROFILE_ZONE();
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
			m_renderer.AddPointLight(Renderer::PointLight{
			        .position = glm::vec3(tc.localToWorld[3]),
			        .radius = l.radius,
			        .color = l.color,
			        .intensity = l.intensity,
			        .castsShadow = l.castsShadow,
			});
		}

		m_renderer.ClearSpotLights();
		for (const auto e: spots)
		{
			const auto& tc = reg.get<TransformComponent>(e);
			const auto& l = reg.get<SpotLightComponent>(e);
			// Local -Z in world space; normalize strips the transform's scale.
			const glm::vec3 forward = -glm::normalize(glm::vec3(tc.localToWorld[2]));
			m_renderer.AddSpotLight(Renderer::SpotLight{
			        .position = glm::vec3(tc.localToWorld[3]),
			        .radius = l.radius,
			        .direction = forward,
			        .innerAngleRad = l.innerAngleRad,
			        .color = l.color,
			        .intensity = l.intensity,
			        .outerAngleRad = l.outerAngleRad,
			        .castsShadow = l.castsShadow,
			});
		}
	}
} // namespace aether
