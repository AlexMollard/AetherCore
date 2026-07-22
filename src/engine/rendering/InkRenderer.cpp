#include "rendering/InkRenderer.hpp"

#include <cstddef>
#include <cstring>
#include <string>

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		constexpr std::uint32_t kInitialSegmentCapacity = 256;

		struct InkPush
		{
			gpu::DeviceAddress frameConstants;
			gpu::DeviceAddress segments;
			std::uint32_t segmentCount;
			std::uint32_t pad;
			float viewportWidth;
			float viewportHeight;
			glm::vec4 bodyColor;
			glm::vec4 rimColor;
		};

		static_assert(sizeof(InkPush) == 64);
		static_assert(sizeof(InkSegmentGpu) == 32);
	} // namespace

	void InkRenderer::Initialize(GpuDevice& gpu, gpu::Format colorFormat)
	{
		AE_PROFILE_ZONE();
		const gpu::GraphicsPipelineDesc desc{
		        .shaderVfsPath = "shaders://ink_field.spv",
		        .colorFormat = colorFormat,
		        .depthFormat = gpu::Format::Undefined,
		        .depthTestEnable = false,
		        .depthWriteEnable = false,
		        .blendEnable = true,
		        .blendMode = gpu::BlendMode::Alpha,
		        .topology = gpu::PrimitiveTopology::TriangleList,
		        .polygonMode = gpu::PolygonMode::Fill,
		        .cullMode = gpu::CullMode::None,
		        .debugName = "InkRenderer.Field",
		        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
		};
		m_pipeline = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
		if (!m_pipeline.IsValid())
		{
			AE_ERROR(LogCategory::Render, "InkRenderer: failed to create ink field pipeline");
		}
	}

	void InkRenderer::Shutdown()
	{
		AE_PROFILE_ZONE();
		for (Frame& frame: m_frames)
		{
			if (frame.buffer.IsValid())
			{
				gpu::ResourceRegistry::Destroy(frame.buffer);
			}
			frame = Frame{};
		}
		if (m_pipeline.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_pipeline);
			m_pipeline = {};
		}
	}

	void InkRenderer::EnsureCapacity(Frame& frame, std::uint32_t count)
	{
		if (frame.capacity >= count && frame.buffer.IsValid())
		{
			return;
		}
		std::uint32_t capacity = frame.capacity == 0 ? kInitialSegmentCapacity : frame.capacity;
		while (capacity < count)
		{
			capacity *= 2;
		}
		if (frame.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(frame.buffer);
		}
		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(capacity) * sizeof(InkSegmentGpu),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "InkRenderer.Segments",
		};
		frame.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!frame.buffer.IsValid())
		{
			frame = Frame{};
			AE_ERROR(LogCategory::Render, "InkRenderer: failed to allocate {} ink segments", capacity);
			return;
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.buffer);
		frame.mapped = view.mappedPtr;
		frame.address = view.deviceAddress;
		frame.capacity = capacity;
	}

	void InkRenderer::BeginFrame(const RenderInkFrameData& frameData, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();
		Frame& frame = m_frames[frameSlot % kFrames];
		frame.count = 0;
		frame.bodyColor = frameData.bodyColor;
		frame.rimColor = frameData.rimColor;
		if (frameData.segments.empty())
		{
			return;
		}
		const auto count = static_cast<std::uint32_t>(frameData.segments.size());
		EnsureCapacity(frame, count);
		if (!frame.buffer.IsValid())
		{
			return;
		}
		const auto byteSize = static_cast<gpu::DeviceSize>(count) * sizeof(InkSegmentGpu);
		std::memcpy(frame.mapped, frameData.segments.data(), byteSize);
		gpu::ResourceRegistry::FlushMappedBuffer(frame.buffer, 0, byteSize);
		frame.count = count;
	}

	void InkRenderer::EndFrame() {}

	void InkRenderer::RegisterPass(RenderGraph& graph,
	        RGImage color,
	        gpu::Extent2D extent,
	        BindlessManager& bindless,
	        std::string_view name,
	        const FrameConstantsBuffer* frameConstants,
	        const std::atomic<bool>* enabled)
	{
		graph.AddFullscreenPass({
		             .name = std::string(name),
		             .color = color,
		             .extent = extent,
		             .loadOp = gpu::LoadOp::Load,
		     })
		        .Execute(
		                [this, &bindless, extent, frameConstants, enabled](PassContext& ctx)
		                {
			                if (enabled != nullptr && !enabled->load(std::memory_order_relaxed))
			                {
				                return;
			                }
			                Frame& frame = m_frames[ctx.frameSlot % kFrames];
			                if (frame.count == 0 || !m_pipeline.IsValid())
			                {
				                return;
			                }
			                bindless.CmdBindHeaps(ctx.recorder);
			                ctx.recorder.BindPipeline(gpu::ResourceRegistry::ResolvePipeline(m_pipeline).state);
			                const InkPush push{
			                        .frameConstants = frameConstants != nullptr ? frameConstants->GetDeviceAddress(ctx.frameSlot) : ctx.frameConstantsAddr,
			                        .segments = frame.address,
			                        .segmentCount = frame.count,
			                        .pad = 0,
			                        .viewportWidth = static_cast<float>(extent.width),
			                        .viewportHeight = static_cast<float>(extent.height),
			                        .bodyColor = frame.bodyColor,
			                        .rimColor = frame.rimColor,
			                };
			                ctx.recorder.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                ctx.recorder.Draw(6, 1, 0, 0);
		                });
	}
} // namespace aether
