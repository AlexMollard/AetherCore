#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vector>

#include "camera/Camera.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/GpuContracts.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	class LightingManager;
}

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

		// Get BDA addresses for the lighting buffers at the given frame slot.
		[[nodiscard]] DrawContracts::LightingAddresses GetLightingAddresses(std::uint32_t frameSlot) const;

		// Register lighting compute passes (InitTiles + BinLights) in the render
		// graph. Must be called after Initialize() and before the first frame.
		// Passes run on the async compute queue when available.
		void RegisterPasses(RenderGraph& graph);

		// Prepare light data for the render graph passes. Builds the light list,
		// ensures GPU buffers, copies data, and stores per-frame push constants.
		// Call before RenderGraph::Execute() each frame.
		// Returns true if lighting should run (lights exist).
		[[nodiscard]] bool PrepareForRenderGraph(std::uint32_t frameSlot, const Camera& camera, gpu::Extent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);
		[[nodiscard]] bool PrepareForRenderGraph(
		        std::uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj, float nearPlane, gpu::Extent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		// Update the render graph's external buffer handles for the current frame.
		void UpdateBufferHandles(RenderGraph& graph, std::uint32_t frameSlot) const;

		// Accessors for render graph buffer handles (for forward pass declarations).
		[[nodiscard]] RGBuffer GetLightsBufferHandle() const
		{
			return m_rgLights;
		}

		[[nodiscard]] RGBuffer GetTileHeadersBufferHandle() const
		{
			return m_rgTileHeaders;
		}

		[[nodiscard]] RGBuffer GetTileIndicesBufferHandle() const
		{
			return m_rgTileIndices;
		}

		void UpdateForView(
		        std::uint32_t frameSlot, const Camera& camera, gpu::Extent2D extent, FrameConstants& fc, bool enableBinningForView, std::span<const Renderer::PointLight> pointLights = {}, std::span<const Renderer::SpotLight> spotLights = {}) const;

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
			gpu::BufferHandle lightsHandle{};
			gpu::BufferHandle tileHeadersHandle{};
			gpu::BufferHandle tileIndicesHandle{};
			void* lightsMapped = nullptr;
			void* tileHeadersMapped = nullptr;
			void* tileIndicesMapped = nullptr;
			gpu::Buffer lightsBuffer = nullptr;
			gpu::Buffer tileHeadersBuffer = nullptr;
			gpu::Buffer tileIndicesBuffer = nullptr;
			gpu::DeviceAddress lightsDeviceAddr = 0;
			gpu::DeviceAddress tileHeadersDeviceAddr = 0;
			gpu::DeviceAddress tileIndicesDeviceAddr = 0;
			gpu::DeviceSize lightsSize = 0;
			gpu::DeviceSize tileHeadersSize = 0;
			gpu::DeviceSize tileIndicesSize = 0;
			std::size_t lightsCapacity = 0;
			std::size_t headersCapacity = 0;
			std::size_t indicesCapacity = 0;
			std::vector<gpu::BufferHandle> staleBuffers;
		};

		void EnsureBuffers(std::uint32_t frameSlot, std::size_t lightCount, std::size_t tileCount, std::size_t indexCount) const;
		void EnsureComputePipeline() const;

		// Builds GpuLight array from point/spot light spans. Appends to outLights.
		static void BuildLightList(std::vector<GpuLight>& outLights, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		void UpdateForViewCpu(std::uint32_t frameSlot, const Camera& camera, gpu::Extent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights) const;
		static void DisableForView(FrameConstants& fc);

		// Per-frame push constants for the lighting compute passes. Sent via
		// vkCmdPushConstants and consumed by the tiled_light_cull compute shader.
		// Layout must match the shader's [[vk::push_constant]] struct.
		struct LightingComputePush
		{
			gpu::DeviceAddress lightDataAddr = 0;
			gpu::DeviceAddress tileHeadersAddr = 0;
			gpu::DeviceAddress tileLightIndicesAddr = 0;
			glm::mat4 viewProj{1.0f};
			glm::vec4 params0{0.0f}; // x=nearClip, y=pixelScaleY, z=screenW, w=screenH
			glm::uvec4 params1{0u};  // x=tilePx, y=tilesX, z=tilesY, w=lightCount
			glm::uvec4 params2{0u};  // x=maxLightsPerTile
		};

		static_assert(sizeof(LightingComputePush) == 136);
		static_assert(offsetof(LightingComputePush, lightDataAddr) == 0);
		static_assert(offsetof(LightingComputePush, tileHeadersAddr) == 8);
		static_assert(offsetof(LightingComputePush, tileLightIndicesAddr) == 16);
		static_assert(offsetof(LightingComputePush, viewProj) == 24);
		static_assert(offsetof(LightingComputePush, params0) == 88);
		static_assert(offsetof(LightingComputePush, params1) == 104);
		static_assert(offsetof(LightingComputePush, params2) == 120);

		const VulkanContext* m_context = nullptr;
		const Renderer* m_renderer = nullptr;
		GpuDevice* m_device = nullptr;
		mutable gpu::PipelineHandle m_initPipelineHandle;
		mutable gpu::PipelineHandle m_cullPipelineHandle;
		mutable std::array<FrameLightingBuffers, kMaxFramesInFlight> m_buffers;
		bool m_rttBinningEnabled = false;
		bool m_gpuBinningEnabled = false;

		// Render graph integration state.
		mutable std::array<LightingComputePush, kMaxFramesInFlight> m_lightPush{}; // set by PrepareForRenderGraph
		mutable std::array<uint32_t, kMaxFramesInFlight> m_lightTileGroups{};      // dispatch group count
		mutable std::array<uint32_t, kMaxFramesInFlight> m_lightLightGroups{};     // dispatch group count
		mutable std::array<bool, kMaxFramesInFlight> m_lightDataReady{};           // true when PrepareForRenderGraph was called this frame
		RGBuffer m_rgLights{};                                                     // render graph handle for lights buffer
		RGBuffer m_rgTileHeaders{};                                                // render graph handle for tile headers buffer
		RGBuffer m_rgTileIndices{};                                                // render graph handle for tile indices buffer
		bool m_rgPassesRegistered = false;                                         // true after RegisterPasses() called
		std::uint32_t m_maxLightsPerTile = 128;
		static constexpr std::uint32_t kTileSizePx = 16;
	};
} // namespace aether
