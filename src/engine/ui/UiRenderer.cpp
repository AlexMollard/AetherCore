#include "ui/UiRenderer.hpp"

#include <cstring>

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "rendering/RenderGraph.hpp"
#include "scene/World.hpp"
#include "ui/UiDrawBuilder.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether::ui
{
	namespace
	{
		// Push-constant layout - MUST match ShapesPush in shaders/ui_shapes.slang
		// byte-for-byte: float4 screenSize (16B) + DevicePtr<DrawCommandData>
		// (8B - a bare device address under the hood) + 2x uint32 padding (8B).
		struct ShapesPush
		{
			glm::vec4 screenSize; // .xy = viewport dimensions
			std::uint64_t commandData;
			std::uint32_t pad0;
			std::uint32_t pad1;
		};
		static_assert(sizeof(ShapesPush) == 32, "ShapesPush must match shaders/ui_shapes.slang ShapesPush layout");

		constexpr std::uint32_t kInitialCommandCapacity = 256;
	} // namespace

	void UiRenderer::Init(GpuDevice& gpu, gpu::Format colorFormat)
	{
		AE_PROFILE_ZONE();

		// ui_shapes.slang samples the bindless resource heap (g_textures[] /
		// g_linearSampler at set 0, for textured rects + SDF glyphs), so the
		// pipeline needs the descriptor-heap mapping info chained into the
		// VkShaderCreateInfoEXT pNext, exactly like GTAOPass/PostProcessStack/
		// TexturePreview do for their bindless-sampling pipelines.
		const gpu::GraphicsPipelineDesc desc{
		        .shaderVfsPath = "shaders://ui_shapes.spv",
		        .fragmentVfsPath = nullptr, // shares the vertex module (single ui_shapes.spv has both stages)
		        .vertexEntry = "vertexMain",
		        .fragmentEntry = "fragmentMain",
		        .colorFormat = colorFormat,
		        .depthFormat = gpu::Format::Undefined,
		        .depthTestEnable = false,
		        .depthWriteEnable = false,
		        .blendEnable = true,
		        .topology = gpu::PrimitiveTopology::TriangleList,
		        .polygonMode = gpu::PolygonMode::Fill,
		        .cullMode = gpu::CullMode::None,
		        .debugName = "UI.Shapes",
		        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
		};

		m_pipeline = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
		if (!m_pipeline.IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to create ui_shapes pipeline");
		}
	}

	void UiRenderer::Shutdown()
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

	void UiRenderer::EnsureCapacity(Frame& frame, std::uint32_t count)
	{
		if (frame.capacity >= count && frame.buffer.IsValid())
		{
			return;
		}

		std::uint32_t newCapacity = frame.capacity == 0 ? kInitialCommandCapacity : frame.capacity;
		while (newCapacity < count)
		{
			newCapacity *= 2;
		}

		if (frame.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(frame.buffer);
			frame.buffer = {};
			frame.mapped = nullptr;
			frame.address = 0;
		}

		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(newCapacity) * sizeof(UiDrawCommand),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "UI.Commands",
		};
		frame.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!frame.buffer.IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to allocate command buffer ({} commands)", newCapacity);
			frame.capacity = 0;
			return;
		}

		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.buffer);
		frame.mapped = view.mappedPtr;
		frame.address = view.deviceAddress;
		frame.capacity = newCapacity;
	}

	void UiRenderer::BuildFrame(glm::vec2 outputExtent, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();

		Frame& frame = m_frames[frameSlot % kFrames];
		frame.count = 0;
		// Set the extent on the slot unconditionally (even with zero commands),
		// so it always matches whatever this slot's buffer holds when the render
		// thread later executes the pass.
		frame.extent = outputExtent;

		if (m_world == nullptr)
		{
			return;
		}

		ResolveCanvases(*m_world, outputExtent);
		BuildDrawCommands(*m_world, m_scratch);

		const auto count = static_cast<std::uint32_t>(m_scratch.size());
		if (count == 0)
		{
			return;
		}

		EnsureCapacity(frame, count);
		if (!frame.buffer.IsValid())
		{
			return;
		}

		const auto byteSize = static_cast<gpu::DeviceSize>(count) * sizeof(UiDrawCommand);
		std::memcpy(frame.mapped, m_scratch.data(), byteSize);
		gpu::ResourceRegistry::FlushMappedBuffer(frame.buffer, 0, byteSize);
		frame.count = count;
	}

	void UiRenderer::RegisterPass(RenderGraph& graph, RGImage color, gpu::Extent2D extent, BindlessManager& bindless, std::uint32_t frameSlot)
	{
		if (!color.IsValid())
		{
			color = graph.GetSwapchainColor();
		}

		const auto slot = frameSlot % kFrames;

		auto pass = graph.AddPass("$UiOverlay");
		if (extent.width != 0 && extent.height != 0)
		{
			pass.SetExtent(extent);
		}

		pass.WriteColor(color, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .Execute(
		                [this, &bindless, slot](PassContext& ctx)
		                {
			                const Frame& frame = m_frames[slot];
			                if (frame.count == 0 || !m_pipeline.IsValid())
			                {
				                return;
			                }

			                gpu::CommandList& cmd = ctx.recorder;

			                const auto resolved = gpu::ResourceRegistry::ResolvePipeline(m_pipeline);
			                cmd.BindPipeline(const_cast<void*>(resolved.state));
			                bindless.CmdBindHeaps(cmd);

			                const ShapesPush push{
			                        .screenSize = {frame.extent.x, frame.extent.y, 0.f, 0.f},
			                        .commandData = frame.address,
			                        .pad0 = 0,
			                        .pad1 = 0,
			                };
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                cmd.Draw(6, frame.count, 0, 0);
		                });
	}
} // namespace aether::ui
