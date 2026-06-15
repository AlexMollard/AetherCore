#include "rendering/Renderer.hpp"

#include <glm/common.hpp>
#include <utility>

#include "passes/PostProcessStack.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void Renderer::Initialize(PostProcessStack* postProcessStack)
	{
		AE_PROFILE_ZONE();
		m_postProcessStack = postProcessStack;
	}

	void Renderer::SetTonemapMode(TonemapMode mode)
	{
		if (m_postProcessStack)
		{
			m_postProcessStack->SetTonemapMode(mode);
		}
	}

	TonemapMode Renderer::GetTonemapMode() const
	{
		return m_postProcessStack ? m_postProcessStack->GetTonemapMode() : TonemapMode{};
	}

	void Renderer::SetFxaaEnabled(bool enabled)
	{
		if (m_postProcessStack)
		{
			m_postProcessStack->SetFxaaEnabled(enabled);
		}
	}

	bool Renderer::IsFxaaEnabled() const
	{
		return m_postProcessStack ? m_postProcessStack->IsFxaaEnabled() : false;
	}

	void Renderer::SetDirectionalLight(glm::vec3 direction, const float intensity)
	{
		AE_PROFILE_ZONE();
		const float len2 = glm::dot(direction, direction);
		if (len2 < 1e-8f)
		{
			direction = glm::vec3(0.577f, 0.577f, 0.577f);
		}
		else
		{
			direction = glm::normalize(direction);
		}

		m_sunDirectionIntensity = glm::vec4(direction, glm::max(intensity, 0.0f));
	}

	glm::vec3 Renderer::GetDirectionalLightDirection() const
	{
		return glm::vec3(m_sunDirectionIntensity);
	}

	float Renderer::GetDirectionalLightIntensity() const
	{
		return m_sunDirectionIntensity.w;
	}

	void Renderer::SetSunColor(glm::vec3 color)
	{
		color = glm::max(color, glm::vec3(0.0f));
		m_sunColor = glm::vec4(color, 1.0f);
	}

	glm::vec3 Renderer::GetSunColor() const
	{
		return glm::vec3(m_sunColor);
	}

	void Renderer::SetAmbientLight(glm::vec3 color)
	{
		color = glm::max(color, glm::vec3(0.0f));
		m_ambientColor = glm::vec4(color, 1.0f);
	}

	glm::vec3 Renderer::GetAmbientLight() const
	{
		return glm::vec3(m_ambientColor);
	}

	void Renderer::SetSkyGradient(glm::vec3 horizonColor, glm::vec3 zenithColor)
	{
		horizonColor = glm::max(horizonColor, glm::vec3(0.0f));
		zenithColor = glm::max(zenithColor, glm::vec3(0.0f));
		m_skyHorizonColor = glm::vec4(horizonColor, 1.0f);
		m_skyZenithColor = glm::vec4(zenithColor, 1.0f);
	}

	glm::vec3 Renderer::GetSkyHorizonColor() const
	{
		return glm::vec3(m_skyHorizonColor);
	}

	glm::vec3 Renderer::GetSkyZenithColor() const
	{
		return glm::vec3(m_skyZenithColor);
	}

	void Renderer::SetSkyVoidColor(glm::vec3 color)
	{
		color = glm::max(color, glm::vec3(0.0f));
		m_skyVoidColor = glm::vec4(color, 1.0f);
	}

	void Renderer::SetPointLights(std::vector<PointLight> lights)
	{
		AE_PROFILE_ZONE();
		for (PointLight& light: lights)
		{
			light.radius = glm::max(light.radius, 0.01f);
			light.intensity = glm::max(light.intensity, 0.0f);
			light.color = glm::max(light.color, glm::vec3(0.0f));
		}
		m_pointLights = std::move(lights);
	}

	void Renderer::SetPointLightPosition(const std::uint32_t idx, const glm::vec3 position)
	{
		if (idx < m_pointLights.size())
		{
			m_pointLights[idx].position = position;
		}
	}

	void Renderer::ClearPointLights()
	{
		m_pointLights.clear();
	}

	void Renderer::SetSpotLights(std::vector<SpotLight> lights)
	{
		AE_PROFILE_ZONE();
		for (SpotLight& light: lights)
		{
			light.radius = glm::max(light.radius, 0.01f);
			light.intensity = glm::max(light.intensity, 0.0f);
			light.color = glm::max(light.color, glm::vec3(0.0f));
			const float dirLen2 = glm::dot(light.direction, light.direction);
			light.direction = (dirLen2 > 1e-8f) ? glm::normalize(light.direction) : glm::vec3(0.0f, -1.0f, 0.0f);
			light.innerAngleRad = glm::clamp(light.innerAngleRad, 0.01f, 1.54f);
			light.outerAngleRad = glm::clamp(light.outerAngleRad, light.innerAngleRad + 0.01f, 1.55f);
		}
		m_spotLights = std::move(lights);
	}

	void Renderer::SetSpotLightPosition(const std::uint32_t idx, const glm::vec3 position)
	{
		if (idx < m_spotLights.size())
		{
			m_spotLights[idx].position = position;
		}
	}

	void Renderer::ClearSpotLights()
	{
		m_spotLights.clear();
	}

	glm::vec3 Renderer::GetSkyVoidColor() const
	{
		return glm::vec3(m_skyVoidColor);
	}

	gpu::Format Renderer::GetColorFormat() const
	{
		return PostProcessStack::GetForwardColorFormat();
	}

	gpu::Format Renderer::GetDepthFormat() const
	{
		return gpu::Format::D32Sfloat;
	}

	gpu::Extent2D Renderer::GetExtent() const
	{
		// Would come from Swapchain, but we need a reference to it.
		// For now, return a placeholder - this should be updated when we have
		// Swapchain access.
		return gpu::Extent2D{1280, 720};
	}
} // namespace aether
