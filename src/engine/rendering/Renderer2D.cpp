#include "rendering/Renderer2D.hpp"

#include <algorithm>
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
		constexpr std::uint32_t kInitialSpriteCapacity = 256;

		struct SpritePush
		{
			gpu::DeviceAddress frameConstants;
			gpu::DeviceAddress instances;
			float viewportWidth;
			float viewportHeight;
			std::uint32_t instanceOffset;
			std::uint32_t padding;
		};

		static_assert(sizeof(SpritePush) == 32);
		static_assert(sizeof(SpriteRenderInstance) == 136);
		static_assert(offsetof(SpriteRenderInstance, uvRect) == 64);
		static_assert(offsetof(SpriteRenderInstance, color) == 80);
		static_assert(offsetof(SpriteRenderInstance, sizeAndPivot) == 96);
		static_assert(offsetof(SpriteRenderInstance, sortKey) == 112);
		static_assert(offsetof(SpriteRenderInstance, textureIndex) == 120);
		static_assert(offsetof(SpriteRenderInstance, entityId) == 124);
		static_assert(offsetof(SpriteRenderInstance, flags) == 128);
		static_assert(offsetof(SpriteRenderInstance, blendMode) == 132);
	} // namespace

	void Renderer2D::Initialize(GpuDevice& gpu, gpu::Format colorFormat)
	{
		AE_PROFILE_ZONE();
		for (std::uint32_t i = 0; i < kBlendModeCount; ++i)
		{
			const auto mode = static_cast<gpu::BlendMode>(i);
			const gpu::GraphicsPipelineDesc desc{
			        .shaderVfsPath = "shaders://sprite2d.spv",
			        .colorFormat = colorFormat,
			        .depthFormat = gpu::Format::Undefined,
			        .depthTestEnable = false,
			        .depthWriteEnable = false,
			        .blendEnable = mode != gpu::BlendMode::Opaque,
			        .blendMode = mode,
			        .topology = gpu::PrimitiveTopology::TriangleList,
			        .polygonMode = gpu::PolygonMode::Fill,
			        .cullMode = gpu::CullMode::None,
			        .debugName = "Renderer2D.Sprite",
			        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
			};
			m_pipelines[i] = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
			if (!m_pipelines[i].IsValid())
			{
				AE_ERROR(LogCategory::Render, "Renderer2D: failed to create blend pipeline {}", i);
			}
		}
	}

	void Renderer2D::Shutdown()
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
		for (gpu::PipelineHandle& pipeline: m_pipelines)
		{
			if (pipeline.IsValid())
			{
				gpu::ResourceRegistry::Destroy(pipeline);
			}
			pipeline = {};
		}
		m_frameData = nullptr;
	}

	void Renderer2D::EnsureCapacity(Frame& frame, std::uint32_t count)
	{
		if (frame.capacity >= count && frame.buffer.IsValid())
		{
			return;
		}
		std::uint32_t capacity = frame.capacity == 0 ? kInitialSpriteCapacity : frame.capacity;
		while (capacity < count)
		{
			capacity *= 2;
		}
		if (frame.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(frame.buffer);
		}
		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(capacity) * sizeof(SpriteRenderInstance),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "Renderer2D.Instances",
		};
		frame.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!frame.buffer.IsValid())
		{
			frame = Frame{};
			AE_ERROR(LogCategory::Render, "Renderer2D: failed to allocate {} sprite instances", capacity);
			return;
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.buffer);
		frame.mapped = view.mappedPtr;
		frame.address = view.deviceAddress;
		frame.capacity = capacity;
	}

	void Renderer2D::BeginFrame(const Render2DFrameData& frameData, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();
		m_frameData = &frameData;
		Frame& frame = m_frames[frameSlot % kFrames];
		frame.count = 0;
		frame.batches.clear();
		if (frameData.sprites.empty())
		{
			return;
		}
		const auto count = static_cast<std::uint32_t>(frameData.sprites.size());
		EnsureCapacity(frame, count);
		if (!frame.buffer.IsValid())
		{
			return;
		}
		const auto byteSize = static_cast<gpu::DeviceSize>(count) * sizeof(SpriteRenderInstance);
		std::memcpy(frame.mapped, frameData.sprites.data(), byteSize);
		gpu::ResourceRegistry::FlushMappedBuffer(frame.buffer, 0, byteSize);
		frame.count = count;

		for (std::uint32_t i = 0; i < count; ++i)
		{
			const std::uint32_t blend = std::min(frameData.sprites[i].blendMode, kBlendModeCount - 1);
			if (frame.batches.empty() || frame.batches.back().blendMode != blend)
			{
				frame.batches.push_back(DrawBatch{.firstInstance = i, .count = 1, .blendMode = blend});
			}
			else
			{
				++frame.batches.back().count;
			}
		}
	}

	void Renderer2D::EndFrame()
	{
		m_frameData = nullptr;
	}

	void Renderer2D::RegisterPass(RenderGraph& graph,
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
			                if (frame.count == 0)
			                {
				                return;
			                }
			                bindless.CmdBindGlobalResources(ctx.recorder);
			                frame.spritePushData.resize(frame.count);
			                std::size_t pushIndex = 0;
			                for (const DrawBatch& batch: frame.batches)
			                {
				                const gpu::PipelineHandle pipeline = m_pipelines[batch.blendMode];
				                if (!pipeline.IsValid())
				                {
					                pushIndex += batch.count;
					                continue;
				                }
				                ctx.recorder.BindPipeline(gpu::ResourceRegistry::ResolvePipeline(pipeline).state);
				                // Shader-object blend state is dynamic. Keep each sprite in an isolated draw so
				                // changing one component's blend mode cannot affect neighbouring instances.
				                for (std::uint32_t instance = 0; instance < batch.count; ++instance, ++pushIndex)
				                {
					                const SpritePush push{
					                        .frameConstants = frameConstants != nullptr ? frameConstants->GetDeviceAddress(ctx.frameSlot) : ctx.frameConstantsAddr,
					                        .instances = frame.address,
					                        .viewportWidth = static_cast<float>(extent.width),
					                        .viewportHeight = static_cast<float>(extent.height),
					                        .instanceOffset = batch.firstInstance + instance,
					                };
					                std::memcpy(frame.spritePushData[pushIndex].data(), &push, sizeof(push));
					                ctx.recorder.PushDataRaw(0, frame.spritePushData[pushIndex]);
					                ctx.recorder.Draw(6, 1, 0, 0);
				                }
			                }
		                });
	}
} // namespace aether
