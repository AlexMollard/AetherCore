#include "Renderer.hpp"

#include <glm/common.hpp>

#include "PostProcessStack.hpp"
#include "Swapchain.hpp"

namespace aether
{
	void Renderer::Initialize(PostProcessStack* postProcessStack)
	{
		m_postProcessStack = postProcessStack;
	}

	void Renderer::SetTonemapMode(TonemapMode mode)
	{
		if (m_postProcessStack)
			m_postProcessStack->SetTonemapMode(mode);
	}

	TonemapMode Renderer::GetTonemapMode() const
	{
		return m_postProcessStack ? m_postProcessStack->GetTonemapMode() : TonemapMode{};
	}

	void Renderer::SetFxaaEnabled(bool enabled)
	{
		if (m_postProcessStack)
			m_postProcessStack->SetFxaaEnabled(enabled);
	}

	bool Renderer::IsFxaaEnabled() const
	{
		return m_postProcessStack ? m_postProcessStack->IsFxaaEnabled() : false;
	}

	void Renderer::SetDirectionalLight(glm::vec3 direction, const float intensity)
	{
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

	glm::vec3 Renderer::GetSkyVoidColor() const
	{
		return glm::vec3(m_skyVoidColor);
	}

	VkFormat Renderer::GetColorFormat() const
	{
		// This is a static query (from PostProcessStack).
		return PostProcessStack::GetForwardColorFormat();
	}

	VkFormat Renderer::GetDepthFormat() const
	{
		// Would be obtained from Swapchain via AetherCore, but for now return a placeholder.
		// In practice, this is passed to the app via LayerContext during initialization.
		return VK_FORMAT_D32_SFLOAT;
	}

	VkExtent2D Renderer::GetExtent() const
	{
		// Would come from Swapchain, but we need a reference to it.
		// For now, return a placeholder — this should be updated when we have Swapchain access.
		return VkExtent2D{ 1280, 720 };
	}
}
