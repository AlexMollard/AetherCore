#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <numbers>
#include <span>
#include <vector>
#include "gpu/GpuFormat.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class PostProcessStack;
	enum class TonemapMode : std::uint32_t;

	class Renderer
	{
	public:
		struct PointLight
		{
			glm::vec3 position{0.0f};
			float radius = 1.0f;
			glm::vec3 color{1.0f};
			float intensity = 1.0f;
			float sourceRadius = 0.1f;
			bool castsShadow = false;
		};

		struct SpotLight
		{
			glm::vec3 position{0.0f};
			float radius = 1.0f;
			glm::vec3 direction{0.0f, -1.0f, 0.0f};
			float innerAngleRad = 0.35f;
			glm::vec3 color{1.0f};
			float intensity = 1.0f;
			float outerAngleRad = 0.60f;
			float sourceRadius = 0.1f;
			bool castsShadow = false;
		};

		Renderer() = default;
		~Renderer() = default;

		void Initialize(PostProcessStack* postProcessStack);
		void AttachGtaoPass(class GTAOPass* pass);

		void SetTonemapMode(TonemapMode mode);
		[[nodiscard]] TonemapMode GetTonemapMode() const;

		void SetFxaaEnabled(bool enabled);
		[[nodiscard]] bool IsFxaaEnabled() const;

		void SetCullMode(gpu::CullMode mode);
		[[nodiscard]] gpu::CullMode GetCullMode() const;

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

		void SetAmbientLight(glm::vec3 color);
		[[nodiscard]] glm::vec3 GetAmbientLight() const;

		[[nodiscard]] glm::vec4 GetAmbientLightVector() const
		{
			return m_ambientColor;
		}

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

		// Atmospheric height fog: density, height falloff, sun forward-scatter, max opacity.
		// Ambient occlusion knobs, forwarded to the GTAO pass when one is attached.
		void SetContactShadowsEnabled(bool enabled)
		{
			m_contactShadows = enabled;
		}

		[[nodiscard]] bool AreContactShadowsEnabled() const
		{
			return m_contactShadows;
		}

		void SetGtaoEnabled(bool enabled);
		[[nodiscard]] bool IsGtaoEnabled() const;
		void SetGtaoRadius(float radius);
		[[nodiscard]] float GetGtaoRadius() const;
		void SetGtaoStrength(float strength);
		[[nodiscard]] float GetGtaoStrength() const;

		void SetFogParams(glm::vec4 params)
		{
			m_fogParams = params;
		}

		[[nodiscard]] glm::vec4 GetFogParams() const
		{
			return m_fogParams;
		}

		[[nodiscard]] glm::vec4 GetSkyVoidColorVector() const
		{
			return m_skyVoidColor;
		}

		void SetPointLights(std::vector<PointLight> lights);
		void AddPointLight(PointLight light);
		void SetPointLightPosition(std::uint32_t idx, glm::vec3 position);
		void SetPointLightColor(std::uint32_t idx, glm::vec3 color);
		void SetPointLightIntensity(std::uint32_t idx, float intensity);
		void ClearPointLights();

		[[nodiscard]] std::span<const PointLight> GetPointLights() const
		{
			return m_pointLights;
		}

		void SetSpotLights(std::vector<SpotLight> lights);
		void AddSpotLight(SpotLight light);
		void SetSpotLightPosition(std::uint32_t idx, glm::vec3 position);
		void SetSpotLightColor(std::uint32_t idx, glm::vec3 color);
		void SetSpotLightIntensity(std::uint32_t idx, float intensity);
		void ClearSpotLights();

		[[nodiscard]] std::span<const SpotLight> GetSpotLights() const
		{
			return m_spotLights;
		}

		[[nodiscard]] static gpu::Format GetColorFormat();
		[[nodiscard]] static gpu::Format GetDepthFormat();
		[[nodiscard]] static gpu::Extent2D GetExtent();

	private:
		PostProcessStack* m_postProcessStack = nullptr;
		class GTAOPass* m_gtaoPass = nullptr;

		gpu::CullMode m_cullMode = gpu::CullMode::Back;
		glm::vec4 m_sunDirectionIntensity{std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, 3.0f};
		glm::vec4 m_sunColor{1.0f, 0.96f, 0.90f, 1.0f};
		glm::vec4 m_ambientColor{0.03f, 0.04f, 0.06f, 1.0f};
		glm::vec4 m_skyHorizonColor{0.34f, 0.52f, 0.82f, 1.0f};
		glm::vec4 m_skyZenithColor{0.08f, 0.19f, 0.45f, 1.0f};
		glm::vec4 m_skyVoidColor{0.001f, 0.002f, 0.005f, 1.0f};
		glm::vec4 m_fogParams{0.0f, 0.08f, 0.6f, 0.9f};
		bool m_contactShadows = false;
		std::vector<PointLight> m_pointLights;
		std::vector<SpotLight> m_spotLights;
	};
} // namespace aether
