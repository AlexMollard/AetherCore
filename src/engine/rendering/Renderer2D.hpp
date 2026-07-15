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

	class Renderer2D
	{
	public:
		void Initialize(GpuDevice& gpu, gpu::Format colorFormat);
		void Shutdown();
		void BeginFrame(const Render2DFrameData& frameData, std::uint32_t frameSlot);
		void EndFrame();
		void RegisterPass(RenderGraph& graph,
		        RGImage color,
		        gpu::Extent2D extent,
		        BindlessManager& bindless,
		        std::string_view name = "$Renderer2D",
		        const FrameConstantsBuffer* frameConstants = nullptr,
		        const std::atomic<bool>* enabled = nullptr);

		[[nodiscard]] const Render2DFrameData* GetFrameData() const
		{
			return m_frameData;
		}

	private:
		static constexpr std::uint32_t kFrames = kMaxFramesInFlight;
		static constexpr std::uint32_t kBlendModeCount = 4;

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
