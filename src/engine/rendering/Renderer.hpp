#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <vector>
#include "vulkan/volk.hpp"

namespace aether
{
	class PostProcessStack;
	class CameraManager;
	enum class TonemapMode : std::uint32_t;

	// Renderer service - owns all rendering configuration and post-processing.
	class Renderer
	{
	public:
		struct PointLight
		{
			glm::vec3 position{ 0.0f };
			float radius = 1.0f;
			glm::vec3 color{ 1.0f };
			float intensity = 1.0f;
			bool castsShadow = false;
		};

		struct SpotLight
		{
			glm::vec3 position{ 0.0f };
			float radius = 1.0f;
			glm::vec3 direction{ 0.0f, -1.0f, 0.0f };
			float innerAngleRad = 0.35f;
			glm::vec3 color{ 1.0f };
			float intensity = 1.0f;
			float outerAngleRad = 0.60f;
			bool castsShadow = false;
		};

		Renderer() = default;
		~Renderer() = default;

		// Initialize with dependencies (called by AetherCore).
		void Initialize(PostProcessStack* postProcessStack);

		// Tonemap mode control.
		void SetTonemapMode(TonemapMode mode);
		[[nodiscard]] TonemapMode GetTonemapMode() const;

		// FXAA control.
		void SetFxaaEnabled(bool enabled);
		[[nodiscard]] bool IsFxaaEnabled() const;

		// Directional light (key light) controls.
		void SetDirectionalLight(glm::vec3 direction, float intensity);
		[[nodiscard]] glm::vec3 GetDirectionalLightDirection() const;
		[[nodiscard]] float GetDirectionalLightIntensity() const;

		[[nodiscard]] glm::vec4 GetDirectionalLightVector() const
		{
			return m_sunDirectionIntensity;
		}

		void SetSunColor(glm::vec3 color);
		[[nodiscard]] glm::vec3 GetSunColor() const;

		[[nodiscard]] glm::vec4 GetSunColorVector() const
		{
			return m_sunColor;
		}

		// Ambient light controls.
		void SetAmbientLight(glm::vec3 color);
		[[nodiscard]] glm::vec3 GetAmbientLight() const;

		[[nodiscard]] glm::vec4 GetAmbientLightVector() const
		{
			return m_ambientColor;
		}

		// Sky color controls used by the skybox pass.
		void SetSkyGradient(glm::vec3 horizonColor, glm::vec3 zenithColor);
		[[nodiscard]] glm::vec3 GetSkyHorizonColor() const;
		[[nodiscard]] glm::vec3 GetSkyZenithColor() const;

		[[nodiscard]] glm::vec4 GetSkyHorizonColorVector() const
		{
			return m_skyHorizonColor;
		}

		[[nodiscard]] glm::vec4 GetSkyZenithColorVector() const
		{
			return m_skyZenithColor;
		}

		void SetSkyVoidColor(glm::vec3 color);
		[[nodiscard]] glm::vec3 GetSkyVoidColor() const;

		[[nodiscard]] glm::vec4 GetSkyVoidColorVector() const
		{
			return m_skyVoidColor;
		}

		// Local lights for tiled forward shading.
		void SetPointLights(std::vector<PointLight> lights);
		void ClearPointLights();

		[[nodiscard]] std::span<const PointLight> GetPointLights() const
		{
			return m_pointLights;
		}

		void SetSpotLights(std::vector<SpotLight> lights);
		void ClearSpotLights();

		[[nodiscard]] std::span<const SpotLight> GetSpotLights() const
		{
			return m_spotLights;
		}

		// Swapchain format queries for pipeline creation.
		[[nodiscard]] VkFormat GetColorFormat() const;
		[[nodiscard]] VkFormat GetDepthFormat() const;
		[[nodiscard]] VkExtent2D GetExtent() const;

	private:
		PostProcessStack* m_postProcessStack = nullptr;

		// Cached light parameters (also written to frame constants).
		glm::vec4 m_sunDirectionIntensity{ 0.577f, 0.577f, 0.577f, 3.0f };
		glm::vec4 m_sunColor{ 1.0f, 0.96f, 0.90f, 1.0f };
		glm::vec4 m_ambientColor{ 0.03f, 0.04f, 0.06f, 1.0f };
		glm::vec4 m_skyHorizonColor{ 0.34f, 0.52f, 0.82f, 1.0f };
		glm::vec4 m_skyZenithColor{ 0.08f, 0.19f, 0.45f, 1.0f };
		glm::vec4 m_skyVoidColor{ 0.001f, 0.002f, 0.005f, 1.0f };
		std::vector<PointLight> m_pointLights;
		std::vector<SpotLight> m_spotLights;
	};
} // namespace aether
