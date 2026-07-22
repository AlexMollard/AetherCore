#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

#include <glm/vec4.hpp>

#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderGraphTypes.hpp"

namespace aether
{
	class BindlessManager;
	class FrameConstantsBuffer;
	class GpuDevice;
	class RenderGraph;

	// One-pass conjured-ink field. Uploads the live ink segments (RenderInkFrameData) to a storage
	// buffer and draws a single fullscreen quad that SDF-unions them into a continuous wet-ink layer
	// via shaders://ink_field.spv. Mirrors Renderer2D's per-frame buffer + fullscreen-pass structure.
	class InkRenderer
	{
	public:
		void Initialize(GpuDevice& gpu, gpu::Format colorFormat);
		void Shutdown();
		void BeginFrame(const RenderInkFrameData& frameData, std::uint32_t frameSlot);
		void EndFrame();
		void RegisterPass(RenderGraph& graph,
		        RGImage color,
		        gpu::Extent2D extent,
		        BindlessManager& bindless,
		        std::string_view name = "$InkField",
		        const FrameConstantsBuffer* frameConstants = nullptr,
		        const std::atomic<bool>* enabled = nullptr);

	private:
		static constexpr std::uint32_t kFrames = kMaxFramesInFlight;

		struct Frame
		{
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
			std::uint32_t capacity = 0;
			std::uint32_t count = 0;
			glm::vec4 bodyColor{0.0f};
			glm::vec4 rimColor{0.0f};
		};

		void EnsureCapacity(Frame& frame, std::uint32_t count);

		gpu::PipelineHandle m_pipeline{};
		std::array<Frame, kFrames> m_frames{};
	};
} // namespace aether
