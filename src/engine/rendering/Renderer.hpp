#pragma once

#include <optional>
#include <algorithm>
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

		void SetGrade(float contrast, float saturation, float temperature, float tint);
		void SetVignette(float intensity, float roundness);
		void SetBloom(float strength, float filterRadius);
		void SetExposure(float exposure);
		void SetAutoExposure(bool enabled, float key, float speed);
		void SetChromaticAberration(float pixels);
		void SetSharpness(float sharpness);
		void SetFilmGrain(float amount);

		// Camera motion blur. `strength` is the shutter's open fraction of the frame
		// interval (0.5 is the film convention, 0 is off); `maxRadiusPixels` bounds how far
		// a single frame may ever smear.
		void SetMotionBlur(float strength, float maxRadiusPixels);
		void SetFxaaEnabled(bool enabled);
		[[nodiscard]] bool IsFxaaEnabled() const;

		void SetCullMode(std::optional<gpu::CullMode> mode);
		[[nodiscard]] std::optional<gpu::CullMode> GetCullMode() const;

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

		// PS2 object lighting: the scene's own light records (ambient + two directional
		// lights), used by kFlagObjectLit materials instead of the sun. Colours carry the
		// scene's scale; directions point towards their light.
		void SetObjectLights(glm::vec3 ambient, glm::vec3 dir0, glm::vec3 color0, glm::vec3 dir1, glm::vec3 color1)
		{
			m_objectAmbient = glm::vec4(glm::max(ambient, glm::vec3(0.0f)), 1.0f);
			m_objectLight0Direction = glm::vec4(glm::normalize(dir0), 0.0f);
			m_objectLight0Color = glm::vec4(glm::max(color0, glm::vec3(0.0f)), 0.0f);
			m_objectLight1Direction = glm::vec4(glm::normalize(dir1), 0.0f);
			m_objectLight1Color = glm::vec4(glm::max(color1, glm::vec3(0.0f)), 0.0f);
		}

		[[nodiscard]] glm::vec4 GetObjectAmbientVector() const { return m_objectAmbient; }
		[[nodiscard]] glm::vec4 GetObjectLight0DirectionVector() const { return m_objectLight0Direction; }
		[[nodiscard]] glm::vec4 GetObjectLight0ColorVector() const { return m_objectLight0Color; }
		[[nodiscard]] glm::vec4 GetObjectLight1DirectionVector() const { return m_objectLight1Direction; }
		[[nodiscard]] glm::vec4 GetObjectLight1ColorVector() const { return m_objectLight1Color; }

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
		// Screen-space reflections. maxRoughness is the cutoff above which a single ray
		// stops representing the lobe and the sky probe answers instead.
		void SetReflectionsEnabled(bool enabled)
		{
			m_reflections = enabled;
		}

		[[nodiscard]] bool AreReflectionsEnabled() const
		{
			return m_reflections;
		}

		// Global off switch for the volumetric march. Whether it runs at all is the
		// scene's call - a scene with no fog has nothing to march - so this only takes
		// the option away, it never turns it on.
		void SetShadowSplitLambda(float v)
		{
			m_shadowSplitLambda = std::clamp(v, 0.0f, 1.0f);
		}

		[[nodiscard]] float GetShadowSplitLambda() const
		{
			return m_shadowSplitLambda;
		}

		void SetVolumetricsEnabled(bool enabled)
		{
			m_volumetrics = enabled;
		}

		[[nodiscard]] bool AreVolumetricsEnabled() const
		{
			return m_volumetrics;
		}

		void SetReflectionMaxRoughness(float v)
		{
			m_reflectionMaxRoughness = std::clamp(v, 0.0f, 1.0f);
		}

		[[nodiscard]] float GetReflectionMaxRoughness() const
		{
			return m_reflectionMaxRoughness;
		}

		void SetReflectionIntensity(float v)
		{
			m_reflectionIntensity = std::clamp(v, 0.0f, 2.0f);
		}

		[[nodiscard]] float GetReflectionIntensity() const
		{
			return m_reflectionIntensity;
		}

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

		void SetSkyParams(glm::vec4 params)
		{
			m_skyParams = params;
		}

		// How strongly roughness is widened to hide specular aliasing. See
		// graphics.specularFilter; 0 restores the unfiltered highlight.
		void SetSpecularFilter(float strength)
		{
			m_shadingParams.x = strength;
		}

		// Cloud cover over the visible sky. Coverage 0 is a clear sky and costs nothing:
		// the shader returns before sampling any noise.
		void SetClouds(float coverage, float speed)
		{
			m_shadingParams.y = coverage;
			m_shadingParams.z = speed;
		}

		[[nodiscard]] float GetCloudCoverage() const
		{
			return m_shadingParams.y;
		}

		[[nodiscard]] float GetCloudSpeed() const
		{
			return m_shadingParams.z;
		}

		[[nodiscard]] float GetSpecularFilter() const
		{
			return m_shadingParams.x;
		}

		[[nodiscard]] glm::vec4 GetShadingParams() const
		{
			return m_shadingParams;
		}

		[[nodiscard]] glm::vec4 GetSkyParams() const
		{
			return m_skyParams;
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

		// A DEBUG override that forces one cull mode on every draw. Empty by default, and it
		// has to be: a material carries its own cull mode - that is what double_sided sets -
		// and this is applied after the pipeline is bound, so any value here silently replaces
		// every material's choice. It defaulted to Back, which is indistinguishable from "no
		// override" until you author a two-sided material and find it still culled.
		std::optional<gpu::CullMode> m_cullMode;
		glm::vec4 m_sunDirectionIntensity{std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, std::numbers::egamma_v<float>, 3.0f};
		glm::vec4 m_sunColor{1.0f, 0.96f, 0.90f, 1.0f};
		glm::vec4 m_ambientColor{0.03f, 0.04f, 0.06f, 1.0f};
		glm::vec4 m_skyHorizonColor{0.34f, 0.52f, 0.82f, 1.0f};
		glm::vec4 m_skyZenithColor{0.08f, 0.19f, 0.45f, 1.0f};
		glm::vec4 m_skyVoidColor{0.001f, 0.002f, 0.005f, 1.0f};
		glm::vec4 m_objectAmbient{1.0f, 1.0f, 1.0f, 1.0f};
		glm::vec4 m_objectLight0Direction{0.0f, 1.0f, 0.0f, 0.0f};
		glm::vec4 m_objectLight0Color{0.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 m_objectLight1Direction{0.0f, 1.0f, 0.0f, 0.0f};
		glm::vec4 m_objectLight1Color{0.0f, 0.0f, 0.0f, 0.0f};
		glm::vec4 m_fogParams{0.0f, 0.08f, 0.6f, 0.9f};
		glm::vec4 m_skyParams{0.0f, 2.5f, 0.0f, 0.0f};
		glm::vec4 m_shadingParams{1.0f, 0.0f, 0.0f, 0.0f};
		bool m_volumetrics = true;
		float m_shadowSplitLambda = 0.65f;
		bool m_contactShadows = false;
		bool m_reflections = true;
		float m_reflectionMaxRoughness = 0.45f;
		float m_reflectionIntensity = 1.0f;
		std::vector<PointLight> m_pointLights;
		std::vector<SpotLight> m_spotLights;
	};
} // namespace aether
