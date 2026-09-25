#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>
#include <vector>

#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderGraphTypes.hpp"

namespace aether
{
	class BindlessManager;
	class FrameConstantsBuffer;
	class GpuDevice;
	class RenderGraph;

	// Depth-tested camera-facing quads for ParticleEmitterComponents in Billboard3D space.
	// Two passes read the scene depth (read-only): the default one runs after the 3D forward
	// pass, into the scene HDR colour; the display-space one (emitters with display_space)
	// runs after the tonemap, into the gamma-encoded LDR target, so it blends like a PS2 GS.
	// One pipeline per (space, blend mode); instances are drawn back-to-front from the camera.
	class BillboardParticleRenderer
	{
	public:
		void Initialize(GpuDevice& gpu, gpu::Format hdrColorFormat, gpu::Format displayColorFormat, gpu::Format depthFormat);
		void Shutdown();
		// Upload + sort this frame's instances (render thread, inside ExecuteRenderFrame).
		void BeginFrame(const Render2DFrameData& frameData, glm::vec3 cameraPos, std::uint32_t frameSlot);
		void EndFrame();
		// Draws the instances whose kBillboardDisplaySpace bit equals `displaySpace`; `color`
		// must be the target of that space's format.
		void RegisterPass(RenderGraph& graph,
		        RGImage color,
		        RGImage depth,
		        gpu::Extent2D extent,
		        BindlessManager& bindless,
		        std::string_view name = "$BillboardParticles",
		        const FrameConstantsBuffer* frameConstants = nullptr,
		        const std::atomic<bool>* enabled = nullptr,
		        bool displaySpace = false);

	private:
		static constexpr std::uint32_t kFrames = kMaxFramesInFlight;
		static constexpr std::uint32_t kBlendModeCount = 2; // alpha, additive
		static constexpr std::uint32_t kSpaceCount = 2;     // scene HDR, display space

		struct DrawBatch
		{
			std::uint32_t firstInstance = 0;
			std::uint32_t count = 0;
			std::uint32_t blendMode = 0;
			bool displaySpace = false;
		};

		struct Frame
		{
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
			std::uint32_t capacity = 0;
			std::uint32_t count = 0;
			std::vector<DrawBatch> batches;
		};

		void EnsureCapacity(Frame& frame, std::uint32_t count);

		const Render2DFrameData* m_frameData = nullptr;
		std::array<Frame, kFrames> m_frames{};
		std::array<gpu::PipelineHandle, kSpaceCount * kBlendModeCount> m_pipelines{}; // [space * 2 + blend]
	};
} // namespace aether
