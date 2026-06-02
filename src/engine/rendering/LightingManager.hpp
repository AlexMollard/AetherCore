#pragma once

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>

#include "camera/Camera.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/CommandRecorder.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/Renderer.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	class LightingManager
	{
	public:
		void Initialize(const VulkanContext& context);
		void LinkRenderer(const Renderer& renderer);
		void Shutdown();

		// Patch shadow indices into the per-light GPU buffer.
		// shadowIndices is an array of 2x float per light (shadowIndex, shadowStrength)
		// matching the layout of GpuLight::shadowIndex (5th float4).
		void ApplyShadowIndices(std::uint32_t frameSlot, std::span<const glm::vec2> shadowIndices);

		void SetRttBinningEnabled(bool enabled)
		{
			m_rttBinningEnabled = enabled;
		}

		[[nodiscard]] bool IsRttBinningEnabled() const
		{
			return m_rttBinningEnabled;
		}

		void SetGpuBinningEnabled(bool enabled)
		{
			m_gpuBinningEnabled = enabled;
		}

		[[nodiscard]] bool IsGpuBinningEnabled() const
		{
			return m_gpuBinningEnabled;
		}

		[[nodiscard]] VkDescriptorSetLayout GetSetLayout() const;
		[[nodiscard]] VkDescriptorSet GetSet(std::uint32_t frameSlot) const;

		void UpdateForView(std::uint32_t frameSlot, CommandRecorder& cmd, const Camera& camera, GpuExtent2D extent, FrameConstants& fc, bool enableBinningForView, bool isAsyncCompute = false, std::span<const Renderer::PointLight> pointLights = {}, std::span<const Renderer::SpotLight> spotLights = {}) const;

		void EmitAcquireBarriers(std::uint32_t frameSlot, CommandRecorder& graphicsCmd, std::uint32_t srcFamily, std::uint32_t dstFamily) const;

	private:
		struct GpuLight
		{
			glm::vec4 positionRadius{ 0.0f };
			glm::vec4 colorIntensity{ 0.0f };
			glm::vec4 directionType{ 0.0f }; // xyz=dir for spot, w=type (0=point, 1=spot)
			glm::vec4 params{ 0.0f };        // x=innerCos, y=outerCos for spot
			glm::vec4 shadowIndex{ -1.0f, 0.0f, 0.0f, 0.0f }; // x=shadow index, y=shadow strength
		};

		struct TileHeader
		{
			std::uint32_t offset = 0;
			std::uint32_t count = 0;
		};

		struct FrameLightingBuffers
		{
			UniqueBuffer lights;
			UniqueBuffer tileHeaders;
			UniqueBuffer tileIndices;
			std::size_t lightsCapacity = 0;
			std::size_t headersCapacity = 0;
			std::size_t indicesCapacity = 0;
		};

		void EnsureBuffers(std::uint32_t frameSlot, std::size_t lightCount, std::size_t tileCount, std::size_t indexCount) const;
		void EnsureComputePipeline() const;
		void UpdateForViewCpu(std::uint32_t frameSlot, const Camera& camera, GpuExtent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights) const;
		void UpdateForViewGpu(std::uint32_t frameSlot, CommandRecorder& cmd, const Camera& camera, GpuExtent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights) const;
		void UpdateDescriptorSet(std::uint32_t frameSlot) const;
		void DisableForView(FrameConstants& fc) const;

		const VulkanContext* m_context = nullptr;
		const Renderer* m_renderer = nullptr;
		VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
		mutable VkPipelineLayout m_computeLayout = VK_NULL_HANDLE;
		mutable VkPipeline m_initPipeline = VK_NULL_HANDLE;
		mutable VkPipeline m_cullPipeline = VK_NULL_HANDLE;
		VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, kMaxFramesInFlight> m_sets{};
		mutable std::array<FrameLightingBuffers, kMaxFramesInFlight> m_buffers;
		bool m_rttBinningEnabled = false;
		bool m_gpuBinningEnabled = true;
		std::uint32_t m_maxLightsPerTile = 128;
		static constexpr std::uint32_t kTileSizePx = 16;
	};
} // namespace aether
