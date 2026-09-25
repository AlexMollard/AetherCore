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
	// Runs after the 3D forward pass (reads the scene depth, read-only) and before post
	// processing, into the scene HDR colour. One pipeline per blend mode; instances are
	// drawn back-to-front from the camera.
	class BillboardParticleRenderer
	{
	public:
		void Initialize(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat);
		void Shutdown();
		// Upload + sort this frame's instances (render thread, inside ExecuteRenderFrame).
		void BeginFrame(const Render2DFrameData& frameData, glm::vec3 cameraPos, std::uint32_t frameSlot);
		void EndFrame();
		void RegisterPass(RenderGraph& graph,
		        RGImage color,
		        RGImage depth,
		        gpu::Extent2D extent,
		        BindlessManager& bindless,
		        std::string_view name = "$BillboardParticles",
		        const FrameConstantsBuffer* frameConstants = nullptr,
		        const std::atomic<bool>* enabled = nullptr);

	private:
		static constexpr std::uint32_t kFrames = kMaxFramesInFlight;
		static constexpr std::uint32_t kBlendModeCount = 2; // alpha, additive

		struct DrawBatch
		{
			std::uint32_t firstInstance = 0;
			std::uint32_t count = 0;
			std::uint32_t blendMode = 0;
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
		std::array<gpu::PipelineHandle, kBlendModeCount> m_pipelines{};
	};
} // namespace aether
