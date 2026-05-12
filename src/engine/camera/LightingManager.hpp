#pragma once

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>
#include "vulkan/volk.hpp"

#include "camera/Camera.hpp"
#include "rendering/CommandRecorder.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/Renderer.hpp"
#include "vulkan/Swapchain.hpp"
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

		void UpdateForView(std::uint32_t frameSlot, VkCommandBuffer cmd, const Camera& camera, VkExtent2D extent, FrameConstants& fc, bool enableBinningForView, std::uint32_t computeQueueFamily = VK_QUEUE_FAMILY_IGNORED, std::uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED) const;

		// Emits QFOT acquire barriers on the lighting buffers from computeQueueFamily
		// to graphicsQueueFamily. Must be called on the graphics command buffer
		// before any fragment shader reads lighting data, when families differ.
		void EmitAcquireBarriers(std::uint32_t frameSlot, VkCommandBuffer graphicsCmd, std::uint32_t srcFamily, std::uint32_t dstFamily) const;

	private:
		struct GpuLight
		{
			glm::vec4 positionRadius{ 0.0f };
			glm::vec4 colorIntensity{ 0.0f };
			glm::vec4 directionType{ 0.0f }; // xyz=dir for spot, w=type (0=point, 1=spot)
			glm::vec4 params{ 0.0f };        // x=innerCos, y=outerCos for spot
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
		void UpdateForViewCpu(std::uint32_t frameSlot, const Camera& camera, VkExtent2D extent, FrameConstants& fc) const;
		void UpdateForViewGpu(std::uint32_t frameSlot, VkCommandBuffer cmd, const Camera& camera, VkExtent2D extent, FrameConstants& fc, std::uint32_t srcQueueFamily, std::uint32_t dstQueueFamily) const;
		void UpdateDescriptorSet(std::uint32_t frameSlot) const;
		void DisableForView(FrameConstants& fc) const;

		const VulkanContext* m_context = nullptr;
		const Renderer* m_renderer = nullptr;
		VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
		mutable VkPipelineLayout m_computeLayout = VK_NULL_HANDLE;
		mutable VkPipeline m_initPipeline = VK_NULL_HANDLE;
		mutable VkPipeline m_cullPipeline = VK_NULL_HANDLE;
		VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
		std::array<VkDescriptorSet, Swapchain::kMaxFramesInFlight> m_sets{};
		mutable std::array<FrameLightingBuffers, Swapchain::kMaxFramesInFlight> m_buffers;
		bool m_rttBinningEnabled = false;
		bool m_gpuBinningEnabled = true;
		std::uint32_t m_maxLightsPerTile = 128;
		static constexpr std::uint32_t kTileSizePx = 16;
	};
} // namespace aether
