#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

	// Renders project-registered custom passes. Each frame's submissions (RenderCustomPassData) are
	// packed into one storage buffer; RegisterPass is called once per graph stage and draws every
	// submission tagged for that stage as a fullscreen quad running the submission's project shader.
	// The engine is game-agnostic: it binds the raw float4 buffer + params/colours and never inspects
	// what the pass draws. Pipelines are cached per (shader, colour-format) and rebuilt on project
	// shader recompile (mirrors UiRenderer's effect-pipeline retirement).
	class CustomPassRenderer
	{
	public:
		void Initialize(GpuDevice& gpu);
		void Shutdown();
		void BeginFrame(const RenderCustomPassData& frameData, std::uint32_t frameSlot);
		void EndFrame();
		void RegisterPass(RenderGraph& graph,
		        CustomPassStage stage,
		        RGImage color,
		        gpu::Extent2D extent,
		        gpu::Format colorFormat,
		        BindlessManager& bindless,
		        std::string_view name = "$CustomPass",
		        const FrameConstantsBuffer* frameConstants = nullptr,
		        const std::atomic<bool>* enabled = nullptr);

	private:
		static constexpr std::uint32_t kFrames = kMaxFramesInFlight;

		struct DrawRecord
		{
			std::uint32_t offsetVec4 = 0; // start of this pass's data in the frame buffer
			std::uint32_t countVec4 = 0;
			CustomPassStage stage = CustomPassStage::OverScene2D;
			std::string shader;
			glm::vec4 params{0.0f};
			glm::vec4 color0{0.0f};
			glm::vec4 color1{0.0f};
		};

		struct Frame
		{
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
			std::uint32_t capacity = 0; // in float4 elements
			std::vector<DrawRecord> draws;
		};

		struct RetiringPipeline
		{
			gpu::PipelineHandle pipeline{};
			int framesLeft = 0;
		};

		void EnsureCapacity(Frame& frame, std::uint32_t vec4Count);
		[[nodiscard]] gpu::PipelineHandle GetPipeline(const std::string& shader, gpu::Format format);
		void RetireStalePipelinesIfShadersChanged();

		GpuDevice* m_gpu = nullptr;
		std::array<Frame, kFrames> m_frames{};
		std::unordered_map<std::string, gpu::PipelineHandle> m_pipelines; // key: "<shader>#<format>"
		std::vector<RetiringPipeline> m_retiring;
		std::uint64_t m_shaderGen = 0;
	};
} // namespace aether
