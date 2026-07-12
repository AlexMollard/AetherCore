#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>
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

	// Stable handle for a light-binning view (a camera POV that shades local
	// lights). View 0 is always the main camera; secondary views (camera preview,
	// RTT camera targets, future reflection probes / split-screen) register their
	// own so every POV gets tile lists culled against ITS frustum - the tile grid
	// is screen-space and only valid for the view that binned it.
	using LightViewId = std::uint32_t;
	inline constexpr LightViewId kMainLightView = 0u;
	inline constexpr LightViewId kInvalidLightView = 0xFFFFFFFFu;

	class LightingManager
	{
	public:
		static constexpr std::uint32_t kMaxLightViews = 8u;

		void Initialize(GpuDevice& device, const VulkanContext& context);
		void LinkRenderer(const Renderer& renderer);
		void Shutdown();

		// Patch shadow indices into the per-light GPU buffer.
		// shadowIndices is an array of 2x float per light (shadowIndex, shadowStrength)
		// matching the layout of GpuLight::shadowIndex (5th float4).
		void ApplyShadowIndices(std::uint32_t frameSlot, std::span<const glm::vec2> shadowIndices);

		// Get BDA addresses for the lighting buffers at the given frame slot.
		// The light-data buffer is shared by all views; the tile buffers are the
		// requested view's. The slot-only overload is the main view.
		[[nodiscard]] DrawContracts::LightingAddresses GetLightingAddresses(std::uint32_t frameSlot) const;
		[[nodiscard]] DrawContracts::LightingAddresses GetLightingAddresses(LightViewId viewId, std::uint32_t frameSlot) const;

		// Register / release a secondary binning view. Returns kInvalidLightView
		// when the fixed pool (kMaxLightViews) is exhausted.
		[[nodiscard]] LightViewId RegisterView(std::string debugName);
		void UnregisterView(LightViewId viewId);

		// Register the $Lighting.BinLights compute pass in the render graph. Must be
		// called after Initialize() and on every graph rebuild. The pass dispatches
		// binning for every view prepared this frame that was not already recorded
		// via RecordBinLights.
		void RegisterPasses(RenderGraph& graph);

		// Main-view prepare: uploads the frame's light list (shared by all views)
		// and stages the main camera's binning dispatch. Call before
		// RenderGraph::Execute() each frame, before any PrepareView call.
		// Returns true if lighting should run (lights exist).
		[[nodiscard]] bool PrepareForRenderGraph(std::uint32_t frameSlot, const Camera& camera, gpu::Extent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);
		[[nodiscard]] bool PrepareForRenderGraph(
		        std::uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj, float nearPlane, gpu::Extent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		// Secondary-view prepare: stages a binning dispatch culled against THIS
		// view's frustum, reusing the light list uploaded by the main-view prepare
		// (call after it, same frame slot). Writes the view's tile grid into fc.
		// Returns false (and disables tiled lighting in fc) when there are no
		// lights this frame or the view/extent is invalid. Perspective views only.
		[[nodiscard]] bool PrepareView(LightViewId viewId, std::uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj, float nearPlane, gpu::Extent2D extent, FrameConstants& fc);

		// Record a prepared view's binning dispatch into `cmd` immediately and
		// consume it (so $Lighting.BinLights won't dispatch it again). For passes
		// that own their command context and run their draws in the same graph
		// region (e.g. RTT camera targets). Includes the barriers it needs.
		void RecordBinLights(LightViewId viewId, std::uint32_t frameSlot, gpu::CommandList cmd);

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

		// Shared per-slot light-data upload (CPU written, all views read it).
		struct FrameLightBuffer
		{
			gpu::BufferHandle lightsHandle{};
			void* lightsMapped = nullptr;
			gpu::Buffer lightsBuffer = nullptr;
			gpu::DeviceAddress lightsDeviceAddr = 0;
			gpu::DeviceSize lightsSize = 0;
			std::size_t lightsCapacity = 0;
			std::uint32_t lightCount = 0;
			std::vector<gpu::BufferHandle> staleBuffers;
		};

		// Per-frame push constants for the binLights compute shader. Sent via
		// vkCmdPushConstants; layout must match the shader's push struct.
		struct LightingComputePush
		{
			gpu::DeviceAddress lightDataAddr = 0;
			gpu::DeviceAddress tileHeadersAddr = 0;
			gpu::DeviceAddress tileLightIndicesAddr = 0;
			glm::mat4 view{1.0f};    // world -> view (culling runs in eye space)
			glm::vec4 params0{0.0f}; // x=nearClip, y=1/proj[0][0], z=1/proj[1][1] (signed), w=unused
			glm::uvec4 params1{0u};  // x=tilePx, y=tilesX, z=tilesY, w=lightCount
			glm::uvec4 params2{0u};  // x=maxLightsPerTile, y=screenW, z=screenH, w=unused
		};

		static_assert(sizeof(LightingComputePush) == 136);
		static_assert(offsetof(LightingComputePush, lightDataAddr) == 0);
		static_assert(offsetof(LightingComputePush, tileHeadersAddr) == 8);
		static_assert(offsetof(LightingComputePush, tileLightIndicesAddr) == 16);
		static_assert(offsetof(LightingComputePush, view) == 24);
		static_assert(offsetof(LightingComputePush, params0) == 88);
		static_assert(offsetof(LightingComputePush, params1) == 104);
		static_assert(offsetof(LightingComputePush, params2) == 120);

		// Per-view, per-slot tile buffers (GPU-written by binLights, device-local)
		// plus the staged dispatch for this frame.
		struct ViewSlot
		{
			gpu::BufferHandle headersHandle{};
			gpu::BufferHandle indicesHandle{};
			gpu::Buffer headersBuffer = nullptr;
			gpu::Buffer indicesBuffer = nullptr;
			gpu::DeviceAddress headersAddr = 0;
			gpu::DeviceAddress indicesAddr = 0;
			std::size_t headersCapacity = 0; // element counts
			std::size_t indicesCapacity = 0;
			LightingComputePush push{};
			std::uint32_t tilesX = 0;
			std::uint32_t tilesY = 0;
			bool ready = false; // prepared this frame, awaiting dispatch (consumed on dispatch)
		};

		struct View
		{
			bool registered = false;
			std::string debugName;
			std::array<ViewSlot, kMaxFramesInFlight> slots{};
		};

		void EnsureLightsBuffer(std::uint32_t frameSlot, std::size_t lightCount) const;
		void EnsureViewBuffers(View& view, std::uint32_t slot, std::size_t tileCount, std::size_t indexCount) const;
		void DestroyViewBuffers(View& view);
		void EnsureComputePipeline() const;

		// Fills the view slot's push + fc grid for the given camera. Requires the
		// slot's shared light list to be uploaded already.
		void StageViewDispatch(View& view, std::uint32_t slot, const glm::mat4& viewMat, const glm::mat4& proj, float nearPlane, gpu::Extent2D extent, FrameConstants& fc) const;

		// Builds GpuLight array from point/spot light spans. Appends to outLights.
		static void BuildLightList(std::vector<GpuLight>& outLights, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		static void DisableForView(FrameConstants& fc);

		const VulkanContext* m_context = nullptr;
		const Renderer* m_renderer = nullptr;
		GpuDevice* m_device = nullptr;
		mutable gpu::PipelineHandle m_cullPipelineHandle;
		mutable std::array<FrameLightBuffer, kMaxFramesInFlight> m_lightBuffers;
		mutable std::array<View, kMaxLightViews> m_views{};
		mutable std::array<bool, kMaxFramesInFlight> m_lightDataReady{}; // shared light list uploaded this frame

		RGBuffer m_rgLights{};      // render graph handle for lights buffer
		RGBuffer m_rgTileHeaders{}; // render graph handle for the MAIN view's tile headers
		RGBuffer m_rgTileIndices{}; // render graph handle for the MAIN view's tile indices
		std::uint32_t m_maxLightsPerTile = 128; // must stay <= the shader's MAX_LIGHTS_PER_TILE (128)
		static constexpr std::uint32_t kTileSizePx = 16;
	};
} // namespace aether
