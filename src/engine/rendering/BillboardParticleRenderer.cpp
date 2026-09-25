#include "rendering/BillboardParticleRenderer.hpp"

#include <algorithm>
#include <cstddef>
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
		constexpr std::uint32_t kInitialInstanceCapacity = 256;

		struct BillboardPush
		{
			gpu::DeviceAddress frameConstants;
			gpu::DeviceAddress instances;
			std::uint32_t instanceOffset;
			std::uint32_t padding;
		};

		static_assert(sizeof(BillboardPush) == 24);
		static_assert(sizeof(BillboardParticleInstance) == 64);
		static_assert(offsetof(BillboardParticleInstance, color) == 16);
		static_assert(offsetof(BillboardParticleInstance, uvRect) == 32);
		static_assert(offsetof(BillboardParticleInstance, rotationDegrees) == 48);
		static_assert(offsetof(BillboardParticleInstance, textureIndex) == 52);
		static_assert(offsetof(BillboardParticleInstance, blendMode) == 56);
		static_assert(offsetof(BillboardParticleInstance, entityId) == 60);
	} // namespace

	void BillboardParticleRenderer::Initialize(GpuDevice& gpu, gpu::Format colorFormat, gpu::Format depthFormat)
	{
		AE_PROFILE_ZONE();
		for (std::uint32_t i = 0; i < kBlendModeCount; ++i)
		{
			// 0 = alpha, 1 = additive: the SpriteBlendMode::Additive value the emitter stores,
			// shifted down past Opaque so the pipeline table stays small.
			const auto mode = i == 0 ? gpu::BlendMode::Alpha : gpu::BlendMode::Additive;
			const gpu::GraphicsPipelineDesc desc{
			        .shaderVfsPath = "shaders://particle_billboard.spv",
			        .colorFormat = colorFormat,
			        .depthFormat = depthFormat,
			        .depthTestEnable = true,
			        .depthWriteEnable = false,
			        .depthCompareOp = gpu::CompareOp::LessOrEqual,
			        .blendEnable = true,
			        .blendMode = mode,
			        .topology = gpu::PrimitiveTopology::TriangleList,
			        .polygonMode = gpu::PolygonMode::Fill,
			        .cullMode = gpu::CullMode::None,
			        .debugName = "BillboardParticles",
			        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
			};
			m_pipelines[i] = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
			if (!m_pipelines[i].IsValid())
			{
				AE_ERROR(LogCategory::Render, "BillboardParticleRenderer: failed to create blend pipeline {}", i);
			}
		}
	}

	void BillboardParticleRenderer::Shutdown()
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

	void BillboardParticleRenderer::EnsureCapacity(Frame& frame, std::uint32_t count)
	{
		if (frame.capacity >= count && frame.buffer.IsValid())
		{
			return;
		}
		std::uint32_t capacity = frame.capacity == 0 ? kInitialInstanceCapacity : frame.capacity;
		while (capacity < count)
		{
			if (capacity > (UINT32_MAX >> 1))
			{
				capacity = UINT32_MAX;
				break;
			}
			capacity *= 2;
		}
		if (frame.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(frame.buffer);
		}
		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(capacity) * sizeof(BillboardParticleInstance),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "BillboardParticles.Instances",
		};
		frame.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!frame.buffer.IsValid())
		{
			frame = Frame{};
			AE_ERROR(LogCategory::Render, "BillboardParticleRenderer: failed to allocate {} billboard instances", capacity);
			return;
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.buffer);
		frame.mapped = view.mappedPtr;
		frame.address = view.deviceAddress;
		frame.capacity = capacity;
	}

	void BillboardParticleRenderer::BeginFrame(const Render2DFrameData& frameData, glm::vec3 cameraPos, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();
		m_frameData = &frameData;
		Frame& frame = m_frames[frameSlot % kFrames];
		frame.count = 0;
		frame.batches.clear();
		if (frameData.billboards.empty())
		{
			return;
		}
		const auto count = static_cast<std::uint32_t>(frameData.billboards.size());
		EnsureCapacity(frame, count);
		if (!frame.buffer.IsValid())
		{
			return;
		}

		// Back-to-front from the camera, additive quads first within a distance tie
		// (they composite the same either way; alpha must come after or it cuts a
		// rectangular hole in the glow behind it).
		std::vector<const BillboardParticleInstance*> order;
		order.reserve(count);
		for (const BillboardParticleInstance& instance: frameData.billboards)
		{
			order.push_back(&instance);
		}
		const auto distanceSq = [cameraPos](const BillboardParticleInstance* a)
		{
			const glm::vec3 d = glm::vec3(a->positionSize) - cameraPos;
			return glm::dot(d, d);
		};
		std::stable_sort(order.begin(), order.end(), [&](const BillboardParticleInstance* a, const BillboardParticleInstance* b)
		{
			const bool aAdditive = a->blendMode != 0, bAdditive = b->blendMode != 0;
			if (aAdditive != bAdditive)
			{
				return aAdditive; // additive (1) before alpha (0)
			}
			return distanceSq(a) > distanceSq(b);
		});

		for (std::uint32_t i = 0; i < count; ++i)
		{
			static_cast<BillboardParticleInstance*>(frame.mapped)[i] = *order[i];
		}
		gpu::ResourceRegistry::FlushMappedBuffer(frame.buffer, 0, static_cast<gpu::DeviceSize>(count) * sizeof(BillboardParticleInstance));
		frame.count = count;

		for (std::uint32_t i = 0; i < count; ++i)
		{
			const std::uint32_t blend = std::min(order[i]->blendMode, 1u);
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

	void BillboardParticleRenderer::EndFrame()
	{
		m_frameData = nullptr;
	}

	void BillboardParticleRenderer::RegisterPass(RenderGraph& graph,
	        RGImage color,
	        RGImage depth,
	        gpu::Extent2D extent,
	        BindlessManager& bindless,
	        std::string_view name,
	        const FrameConstantsBuffer* frameConstants,
	        const std::atomic<bool>* enabled)
	{
		auto pass = graph.AddPass(std::string(name));
		pass.SetExtent(extent);
		pass.WriteColor(color, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .WriteDepth(depth, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .Execute(
		                [this, &bindless, frameConstants, enabled](PassContext& ctx)
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
			                for (const DrawBatch& batch: frame.batches)
			                {
				                const gpu::PipelineHandle pipeline = m_pipelines[batch.blendMode];
				                if (!pipeline.IsValid())
				                {
					                continue;
				                }
				                ctx.recorder.BindPipeline(gpu::ResourceRegistry::ResolvePipeline(pipeline).state);
				                const BillboardPush push{
				                        .frameConstants = frameConstants != nullptr ? frameConstants->GetDeviceAddress(ctx.frameSlot) : ctx.frameConstantsAddr,
				                        .instances = frame.address,
				                        .instanceOffset = batch.firstInstance,
				                };
				                ctx.recorder.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                // One quad per instance; the vertex shader builds it from SV_VertexID.
				                ctx.recorder.Draw(6, batch.count, 0, 0);
			                }
		                });
	}
} // namespace aether
