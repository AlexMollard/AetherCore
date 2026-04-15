#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

namespace aether
{
	class PostProcessStack;
	class CameraManager;
	enum class TonemapMode : std::uint32_t;

	// Renderer service — owns all rendering configuration and post-processing.
	class Renderer
	{
	public:
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
		[[nodiscard]] glm::vec4 GetDirectionalLightVector() const { return m_sunDirectionIntensity; }

		// Ambient light controls.
		void SetAmbientLight(glm::vec3 color);
		[[nodiscard]] glm::vec3 GetAmbientLight() const;
		[[nodiscard]] glm::vec4 GetAmbientLightVector() const { return m_ambientColor; }

		// Swapchain format queries for pipeline creation.
		[[nodiscard]] VkFormat GetColorFormat() const;
		[[nodiscard]] VkFormat GetDepthFormat() const;
		[[nodiscard]] VkExtent2D GetExtent() const;

	private:
		PostProcessStack* m_postProcessStack = nullptr;

		// Cached light parameters (also written to frame constants).
		glm::vec4 m_sunDirectionIntensity{ 0.577f, 0.577f, 0.577f, 3.0f };
		glm::vec4 m_ambientColor{ 0.03f, 0.04f, 0.06f, 1.0f };
	};
}
