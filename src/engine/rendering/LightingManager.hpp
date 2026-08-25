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
#include "gpu/PushConstantsBytes.hpp"
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

		// matching the layout of GpuLight::shadowIndex (5th float4).
		void ApplyShadowIndices(std::uint32_t frameSlot, std::span<const glm::vec2> shadowIndices);

		[[nodiscard]] DrawContracts::LightingAddresses GetLightingAddresses(std::uint32_t frameSlot) const;
		[[nodiscard]] DrawContracts::LightingAddresses GetLightingAddresses(LightViewId viewId, std::uint32_t frameSlot) const;

		[[nodiscard]] LightViewId RegisterView(std::string debugName);
		void UnregisterView(LightViewId viewId);

		// Register the $Lighting.BinLights compute pass in the render graph. Must be
		void RegisterPasses(RenderGraph& graph);

		[[nodiscard]] bool PrepareForRenderGraph(std::uint32_t frameSlot, const Camera& camera, gpu::Extent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);
		[[nodiscard]] bool PrepareForRenderGraph(
		        std::uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj, float nearPlane, gpu::Extent2D extent, FrameConstants& fc, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		[[nodiscard]] bool PrepareView(LightViewId viewId, std::uint32_t frameSlot, const glm::mat4& view, const glm::mat4& proj, float nearPlane, gpu::Extent2D extent, FrameConstants& fc);

		void RecordBinLights(LightViewId viewId, std::uint32_t frameSlot, gpu::CommandList cmd);

		void UpdateBufferHandles(RenderGraph& graph, std::uint32_t frameSlot) const;

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
			glm::vec4 directionType{0.0f};
			glm::vec4 params{0.0f};
			glm::vec4 shadowIndex{-1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct TileHeader
		{
			std::uint32_t offset = 0;
			std::uint32_t count = 0;
		};

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

		// vkCmdPushConstants; layout must match the shader's push struct. Slang emits this
		// block in natural (scalar) layout - verified against the SPIR-V Offset decorations -
		// so the members pack tight with no vec4 alignment padding.
		//
		// params0 and params2 used to be a vec4 and a uvec4 with a documented-unused trailing
		// component each. That padding cost 8 bytes and pushed the block to 136, over the
		// 128-byte floor Vulkan guarantees for maxPushConstantsSize - which is exactly what
		// RDNA2 exposes. The block is at that floor now, and the static_assert below is what
		// stops it drifting back over: see kMaxGuaranteedPushConstantSize.
		struct LightingComputePush
		{
			gpu::DeviceAddress lightDataAddr = 0;
			gpu::DeviceAddress tileHeadersAddr = 0;
			gpu::DeviceAddress tileLightIndicesAddr = 0;
			glm::mat4 view{1.0f};
			glm::vec3 params0{0.0f};  // x=nearClip, y=1/proj[0][0], z=1/proj[1][1] (signed)
			glm::uvec4 params1{0u};   // x=tilePx, y=tilesX, z=tilesY, w=lightCount
			glm::uvec3 params2{0u};   // x=maxLightsPerTile, y=screenW(px), z=screenH(px)
		};

		static_assert(sizeof(LightingComputePush) == 128);
		static_assert(sizeof(LightingComputePush) <= gpu::kMaxGuaranteedPushConstantSize,
		        "LightingComputePush exceeds the push-constant size every Vulkan device is guaranteed to support. "
		        "Move a field behind a buffer-device-address pointer rather than raising this bound.");
		static_assert(offsetof(LightingComputePush, lightDataAddr) == 0);
		static_assert(offsetof(LightingComputePush, tileHeadersAddr) == 8);
		static_assert(offsetof(LightingComputePush, tileLightIndicesAddr) == 16);
		static_assert(offsetof(LightingComputePush, view) == 24);
		static_assert(offsetof(LightingComputePush, params0) == 88);
		static_assert(offsetof(LightingComputePush, params1) == 100);
		static_assert(offsetof(LightingComputePush, params2) == 116);

		struct ViewSlot
		{
			gpu::BufferHandle headersHandle{};
			gpu::BufferHandle indicesHandle{};
			gpu::Buffer headersBuffer = nullptr;
			gpu::Buffer indicesBuffer = nullptr;
			gpu::DeviceAddress headersAddr = 0;
			gpu::DeviceAddress indicesAddr = 0;
			std::size_t headersCapacity = 0;
			std::size_t indicesCapacity = 0;
			LightingComputePush push{};
			std::uint32_t tilesX = 0;
			std::uint32_t tilesY = 0;
			bool ready = false;
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

		void StageViewDispatch(View& view, std::uint32_t slot, const glm::mat4& viewMat, const glm::mat4& proj, float nearPlane, gpu::Extent2D extent, FrameConstants& fc) const;

		static void BuildLightList(std::vector<GpuLight>& outLights, std::span<const Renderer::PointLight> pointLights, std::span<const Renderer::SpotLight> spotLights);

		static void DisableForView(FrameConstants& fc);

		const VulkanContext* m_context = nullptr;
		const Renderer* m_renderer = nullptr;
		GpuDevice* m_device = nullptr;
		mutable gpu::PipelineHandle m_cullPipelineHandle;
		mutable std::array<FrameLightBuffer, kMaxFramesInFlight> m_lightBuffers;
		mutable std::array<View, kMaxLightViews> m_views{};
		mutable std::array<bool, kMaxFramesInFlight> m_lightDataReady{};

		RGBuffer m_rgLights{};
		RGBuffer m_rgTileHeaders{};
		RGBuffer m_rgTileIndices{};
		std::uint32_t m_maxLightsPerTile = 128; // must stay <= the shader's MAX_LIGHTS_PER_TILE (128)
		static constexpr std::uint32_t kTileSizePx = 16;
	};
} // namespace aether
