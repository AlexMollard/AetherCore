#include "rendering/CustomPassRenderer.hpp"

#include <cstddef>
#include <cstring>
#include <string>

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "io/FileSystem.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		constexpr std::uint32_t kInitialVec4Capacity = 512;

		// MUST match the generic CustomPass push in project shaders.
		struct CustomPassPush
		{
			gpu::DeviceAddress frameConstants;
			gpu::DeviceAddress data;
			std::uint32_t count; // float4 elements bound at `data`
			std::uint32_t pad;
			float viewportWidth;
			float viewportHeight;
			glm::vec4 params;
			glm::vec4 color0;
			glm::vec4 color1;
		};

		static_assert(sizeof(CustomPassPush) == 80);

		std::string PipelineKey(const std::string& shader, gpu::Format format)
		{
			return shader + "#" + std::to_string(static_cast<int>(format));
		}
	} // namespace

	void CustomPassRenderer::Initialize(GpuDevice& gpu)
	{
		m_gpu = &gpu;
	}

	void CustomPassRenderer::Shutdown()
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
		for (auto& [key, pipe]: m_pipelines)
		{
			if (pipe.IsValid())
			{
				gpu::ResourceRegistry::Destroy(pipe);
			}
		}
		m_pipelines.clear();
		for (RetiringPipeline& r: m_retiring)
		{
			if (r.pipeline.IsValid())
			{
				gpu::ResourceRegistry::Destroy(r.pipeline);
			}
		}
		m_retiring.clear();
		m_gpu = nullptr;
	}

	void CustomPassRenderer::EnsureCapacity(Frame& frame, std::uint32_t vec4Count)
	{
		if (frame.capacity >= vec4Count && frame.buffer.IsValid())
		{
			return;
		}
		std::uint32_t capacity = frame.capacity == 0 ? kInitialVec4Capacity : frame.capacity;
		while (capacity < vec4Count)
		{
			capacity *= 2;
		}
		if (frame.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(frame.buffer);
		}
		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(capacity) * sizeof(glm::vec4),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "CustomPass.Data",
		};
		frame.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!frame.buffer.IsValid())
		{
			frame.mapped = nullptr;
			frame.address = 0;
			frame.capacity = 0;
			AE_ERROR(LogCategory::Render, "CustomPassRenderer: failed to allocate {} float4s", capacity);
			return;
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.buffer);
		frame.mapped = view.mappedPtr;
		frame.address = view.deviceAddress;
		frame.capacity = capacity;
	}

	void CustomPassRenderer::RetireStalePipelinesIfShadersChanged()
	{
		if (const std::uint64_t gen = io::FileSystem::ShaderOverlayGeneration(); gen != m_shaderGen)
		{
			m_shaderGen = gen;
			for (auto& [key, pipe]: m_pipelines)
			{
				if (pipe.IsValid())
				{
					m_retiring.push_back({pipe, static_cast<int>(kFrames) + 1});
				}
			}
			m_pipelines.clear();
		}
		for (std::size_t i = 0; i < m_retiring.size();)
		{
			if (--m_retiring[i].framesLeft <= 0)
			{
				gpu::ResourceRegistry::Destroy(m_retiring[i].pipeline);
				m_retiring[i] = m_retiring.back();
				m_retiring.pop_back();
			}
			else
			{
				++i;
			}
		}
	}

	gpu::PipelineHandle CustomPassRenderer::GetPipeline(const std::string& shader, gpu::Format format)
	{
		const std::string key = PipelineKey(shader, format);
		if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
		{
			return it->second;
		}

		gpu::PipelineHandle handle{};
		if (m_gpu != nullptr && !shader.empty())
		{
			const std::string path = "shaders://" + shader + ".spv";
			const gpu::GraphicsPipelineDesc desc{
			        .shaderVfsPath = path.c_str(),
			        .vertexEntry = "vertexMain",
			        .fragmentEntry = "fragmentMain",
			        .colorFormat = format,
			        .depthFormat = gpu::Format::Undefined,
			        .depthTestEnable = false,
			        .depthWriteEnable = false,
			        .blendEnable = true,
			        .blendMode = gpu::BlendMode::Alpha,
			        .topology = gpu::PrimitiveTopology::TriangleList,
			        .polygonMode = gpu::PolygonMode::Fill,
			        .cullMode = gpu::CullMode::None,
			        .debugName = "CustomPass",
			        .descriptorHeapMappings = m_gpu->GetBindlessManager().GetDescriptorHeapMappings(),
			};
			handle = gpu::ResourceRegistry::CreateGraphicsPipeline(m_gpu->GetDevice(), desc);
			if (!handle.IsValid())
			{
				AE_ERROR(LogCategory::Render, "CustomPassRenderer: failed to create pipeline for '{}'", shader);
			}
		}
		// Cache even an invalid handle so a missing shader is not retried (and re-logged) every frame.
		m_pipelines.emplace(key, handle);
		return handle;
	}

	void CustomPassRenderer::BeginFrame(const RenderCustomPassData& frameData, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();
		RetireStalePipelinesIfShadersChanged();

		Frame& frame = m_frames[frameSlot % kFrames];
		frame.draws.clear();
		if (frameData.passes.empty())
		{
			return;
		}

		std::uint32_t total = 0;
		for (const CustomPassDraw& p: frameData.passes)
		{
			total += static_cast<std::uint32_t>(p.data.size());
		}
		if (total == 0)
		{
			// Passes may legitimately submit no data (e.g. a fullscreen shader with only params); still
			// record them so the pass runs, just with a zero-length buffer bound.
			for (const CustomPassDraw& p: frameData.passes)
			{
				frame.draws.push_back({0, 0, p.stage, p.shader, p.params, p.color0, p.color1});
			}
			return;
		}

		EnsureCapacity(frame, total);
		if (!frame.buffer.IsValid())
		{
			return;
		}

		auto* dst = static_cast<glm::vec4*>(frame.mapped);
		std::uint32_t offset = 0;
		for (const CustomPassDraw& p: frameData.passes)
		{
			const auto count = static_cast<std::uint32_t>(p.data.size());
			if (count > 0)
			{
				std::memcpy(dst + offset, p.data.data(), static_cast<std::size_t>(count) * sizeof(glm::vec4));
			}
			frame.draws.push_back({offset, count, p.stage, p.shader, p.params, p.color0, p.color1});
			offset += count;
		}
		gpu::ResourceRegistry::FlushMappedBuffer(frame.buffer, 0, static_cast<gpu::DeviceSize>(total) * sizeof(glm::vec4));
	}

	void CustomPassRenderer::EndFrame() {}

	void CustomPassRenderer::RegisterPass(RenderGraph& graph,
	        CustomPassStage stage,
	        RGImage color,
	        gpu::Extent2D extent,
	        gpu::Format colorFormat,
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
		                [this, stage, colorFormat, &bindless, extent, frameConstants, enabled](PassContext& ctx)
		                {
			                if (enabled != nullptr && !enabled->load(std::memory_order_relaxed))
			                {
				                return;
			                }
			                Frame& frame = m_frames[ctx.frameSlot % kFrames];
			                bool boundHeaps = false;
			                for (const DrawRecord& d: frame.draws)
			                {
				                if (d.stage != stage)
				                {
					                continue;
				                }
				                const gpu::PipelineHandle pipe = GetPipeline(d.shader, colorFormat);
				                if (!pipe.IsValid())
				                {
					                continue;
				                }
				                if (!boundHeaps)
				                {
					                bindless.CmdBindGlobalResources(ctx.recorder);
					                boundHeaps = true;
				                }
				                ctx.recorder.BindPipeline(gpu::ResourceRegistry::ResolvePipeline(pipe).state);
				                const CustomPassPush push{
				                        .frameConstants = frameConstants != nullptr ? frameConstants->GetDeviceAddress(ctx.frameSlot) : ctx.frameConstantsAddr,
				                        .data = frame.address + static_cast<gpu::DeviceAddress>(d.offsetVec4) * sizeof(glm::vec4),
				                        .count = d.countVec4,
				                        .pad = 0,
				                        .viewportWidth = static_cast<float>(extent.width),
				                        .viewportHeight = static_cast<float>(extent.height),
				                        .params = d.params,
				                        .color0 = d.color0,
				                        .color1 = d.color1,
				                };
				                ctx.recorder.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                ctx.recorder.Draw(6, 1, 0, 0);
			                }
		                });
	}
} // namespace aether
