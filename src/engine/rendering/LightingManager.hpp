#pragma once

#include <array>
#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>

#include "camera/Camera.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/DescriptorSetLayout.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderGraph.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	class RenderGraph;
	struct RGBuffer;

	class LightingManager
	{
	public:
		void Initialize(GpuDevice& device, const VulkanContext& context);
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

		[[nodiscard]] gpu::DescriptorSetLayout GetSetLayout() const;
		// Push lighting descriptors (3 storage buffers) directly into the command list
		// at setIndex in the given pipeline layout. Replaces per-frame VkDescriptorSet allocation.
		// The layout pointer is the raw VkPipelineLayout (kept as void* so the
		// engine-facing signature doesn't expose Vk*).
		void PushLightingDescriptor(gpu::CommandList& cmd, void* layout, std::uint32_t frameSlot) const;

		// Register lighting compute passes (InitTiles + BinLights) in the render
		// graph. Must be called after Initialize() and before the first frame.
		// Passes run on the async compute queue when available.
		void RegisterPasses(RenderGraph& graph);

		// Prepare light data for the render graph passes. Builds the light list,
		// ensures GPU buffers, copies data, and stores per-frame push constants.
		// Call before RenderGraph::Execute() each frame.
		// Returns true if lighting should run (lights exist).
		[[nodiscard]] bool PrepareForRenderGraph(std::uint32_t frameSlot, const Camera& camera, GpuExtent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		// Update the render graph's external buffer handles for the current frame.
		void UpdateBufferHandles(RenderGraph& graph, std::uint32_t frameSlot) const;

		void UpdateForView(std::uint32_t frameSlot,
		        gpu::CommandList& cmd,
		        const Camera& camera,
		        GpuExtent2D extent,
		        FrameConstants& fc,
		        bool enableBinningForView,
		        bool isAsyncCompute = false,
		        std::span<const Renderer::PointLight> pointLights = {},
		        std::span<const Renderer::SpotLight> spotLights = {}) const;

		void EmitAcquireBarriers(std::uint32_t frameSlot, gpu::CommandList& graphicsCmd, std::uint32_t srcFamily, std::uint32_t dstFamily) const;

	private:
		struct GpuLight
		{
			glm::vec4 positionRadius{0.0f};
			glm::vec4 colorIntensity{0.0f};
			glm::vec4 directionType{0.0f};                  // xyz=dir for spot, w=type (0=point, 1=spot)
			glm::vec4 params{0.0f};                         // x=innerCos, y=outerCos for spot
			glm::vec4 shadowIndex{-1.0f, 0.0f, 0.0f, 0.0f}; // x=shadow index, y=shadow strength
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
			std::vector<UniqueBuffer> staleBuffers;
		};

		void EnsureBuffers(std::uint32_t frameSlot, std::size_t lightCount, std::size_t tileCount, std::size_t indexCount) const;
		void EnsureComputePipeline() const;

		// Builds GpuLight array from point/spot light spans. Appends to outLights.
		static void BuildLightList(std::vector<GpuLight>& outLights, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		void UpdateForViewCpu(std::uint32_t frameSlot, const Camera& camera, GpuExtent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights) const;
		void UpdateForViewGpu(std::uint32_t frameSlot, gpu::CommandList& cmd, const Camera& camera, GpuExtent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights) const;
		void DisableForView(FrameConstants& fc) const;

		// Per-frame push constants for the lighting compute passes (filled by
		// PrepareForRenderGraph, consumed by the render graph pass callback).
		struct LightingComputePush
		{
			glm::mat4 viewProj{1.0f};
			glm::vec4 params0{0.0f};
			glm::uvec4 params1{0u};
			glm::uvec4 params2{0u};
		};

		const VulkanContext* m_context = nullptr;
		const Renderer* m_renderer = nullptr;
		GpuDevice* m_device = nullptr;
		gpu::DescriptorSetLayout m_setLayout = nullptr;
		mutable gpu::PipelineLayout m_computeLayout = nullptr;
		mutable gpu::PipelineHandle m_initPipelineHandle;
		mutable gpu::PipelineHandle m_cullPipelineHandle;
		mutable std::array<FrameLightingBuffers, kMaxFramesInFlight> m_buffers;
		bool m_rttBinningEnabled = false;
		bool m_gpuBinningEnabled = false;

		// Render graph integration state.
		mutable LightingComputePush m_lightPush; // set by PrepareForRenderGraph
		mutable uint32_t m_lightTileGroups = 0;  // dispatch group count
		mutable uint32_t m_lightLightGroups = 0; // dispatch group count
		mutable bool m_lightDataReady = false;   // true when PrepareForRenderGraph was called this frame
		RGBuffer m_rgLights{};                   // render graph handle for lights buffer
		RGBuffer m_rgTileHeaders{};              // render graph handle for tile headers buffer
		RGBuffer m_rgTileIndices{};              // render graph handle for tile indices buffer
		bool m_rgPassesRegistered = false;       // true after RegisterPasses() called
		std::uint32_t m_maxLightsPerTile = 128;
		static constexpr std::uint32_t kTileSizePx = 16;
	};
} // namespace aether
