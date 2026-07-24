#include "rendering/Light2DCompositor.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

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
		constexpr std::uint32_t kInitialLightCapacity = 64;
		constexpr float kLightClampCeiling = 4.0f; // multiply ceiling: over-bright stacks saturate

		// MUST match Light2DPush in shaders/light2d.slang (48 bytes).
		struct Light2DPush
		{
			gpu::DeviceAddress frameConstants;
			gpu::DeviceAddress lights;
			std::uint32_t count;
			std::uint32_t pad;
			float viewportWidth;
			float viewportHeight;
			glm::vec4 ambient;
		};

		static_assert(sizeof(Light2DPush) == 48);
		static_assert(sizeof(GpuLight2D) == 48);
	} // namespace

	void Light2DCompositor::Initialize(GpuDevice& gpu, gpu::Format colorFormat)
	{
		AE_PROFILE_ZONE();
		const gpu::GraphicsPipelineDesc desc{
		        .shaderVfsPath = "shaders://light2d.spv",
		        .colorFormat = colorFormat,
		        .depthFormat = gpu::Format::Undefined,
		        .depthTestEnable = false,
		        .depthWriteEnable = false,
		        .blendEnable = true,
		        .blendMode = gpu::BlendMode::Multiply,
		        .topology = gpu::PrimitiveTopology::TriangleList,
		        .polygonMode = gpu::PolygonMode::Fill,
		        .cullMode = gpu::CullMode::None,
		        .debugName = "Light2DCompositor",
		        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
		};
		m_pipeline = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
		if (!m_pipeline.IsValid())
		{
			AE_ERROR(LogCategory::Render, "Light2DCompositor: failed to create pipeline");
		}
	}

	void Light2DCompositor::Shutdown()
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

	void Light2DCompositor::EnsureCapacity(Frame& frame, std::uint32_t count)
	{
		if (frame.capacity >= count && frame.buffer.IsValid())
		{
			return;
		}
		std::uint32_t capacity = frame.capacity == 0 ? kInitialLightCapacity : frame.capacity;
		while (capacity < count)
		{
			capacity *= 2;
		}
		if (frame.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(frame.buffer);
		}
		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(capacity) * sizeof(GpuLight2D),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "Light2D.Lights",
		};
		frame.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!frame.buffer.IsValid())
		{
			frame.mapped = nullptr;
			frame.address = 0;
			frame.capacity = 0;
			AE_ERROR(LogCategory::Render, "Light2DCompositor: failed to allocate {} lights", capacity);
			return;
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.buffer);
		frame.mapped = view.mappedPtr;
		frame.address = view.deviceAddress;
		frame.capacity = capacity;
	}

	void Light2DCompositor::BeginFrame(const RenderFramePacket& packet, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();
		Frame& frame = m_frames[frameSlot % kFrames];
		frame.count = 0;

		// Content-driven gate, NOT scene metadata: this project's scenes serialize every SceneFeatureFlag
		// regardless of kind ("every feature in every scene"), so the flags can't tell 2D from 3D. Instead
		// engage only when there IS a 2D layer to modulate - sprites/tiles present this frame. A pure-3D
		// frame has none, so it self-skips and its meshes (lit by the 3D renderer) are never double-dimmed.
		if (packet.render2D.sprites.empty())
		{
			return;
		}
		if (packet.pointLights.empty() && packet.spotLights.empty())
		{
			return;
		}

		std::vector<GpuLight2D> lights;
		lights.reserve(packet.pointLights.size() + packet.spotLights.size());
		for (const Renderer::PointLight& l: packet.pointLights)
		{
			GpuLight2D g;
			g.posRadiusKind = glm::vec4(l.position.x, l.position.y, l.radius, 0.0f);
			g.colorIntensity = glm::vec4(l.color, l.intensity);
			g.spotDirCone = glm::vec4(0.0f);
			lights.push_back(g);
		}
		for (const Renderer::SpotLight& l: packet.spotLights)
		{
			glm::vec2 dir(l.direction.x, l.direction.y);
			const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
			dir = len > 1e-5f ? dir / len : glm::vec2(1.0f, 0.0f);
			GpuLight2D g;
			g.posRadiusKind = glm::vec4(l.position.x, l.position.y, l.radius, 1.0f);
			g.colorIntensity = glm::vec4(l.color, l.intensity);
			g.spotDirCone = glm::vec4(dir.x, dir.y, std::cos(l.innerAngleRad), std::cos(l.outerAngleRad));
			lights.push_back(g);
		}

		const auto count = static_cast<std::uint32_t>(lights.size());
		EnsureCapacity(frame, count);
		if (!frame.buffer.IsValid())
		{
			return;
		}
		const auto byteSize = static_cast<gpu::DeviceSize>(count) * sizeof(GpuLight2D);
		std::memcpy(frame.mapped, lights.data(), byteSize);
		gpu::ResourceRegistry::FlushMappedBuffer(frame.buffer, 0, byteSize);
		frame.count = count;
		frame.ambient = glm::vec4(glm::vec3(packet.ambientColor), kLightClampCeiling);
	}

	void Light2DCompositor::EndFrame() {}

	void Light2DCompositor::RegisterPass(RenderGraph& graph,
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
			                if (frame.count == 0 || !m_pipeline.IsValid() || !frame.buffer.IsValid())
			                {
				                return;
			                }
			                bindless.CmdBindHeaps(ctx.recorder);
			                ctx.recorder.BindPipeline(gpu::ResourceRegistry::ResolvePipeline(m_pipeline).state);
			                const Light2DPush push{
			                        .frameConstants = frameConstants != nullptr ? frameConstants->GetDeviceAddress(ctx.frameSlot) : ctx.frameConstantsAddr,
			                        .lights = frame.address,
			                        .count = frame.count,
			                        .pad = 0,
			                        .viewportWidth = static_cast<float>(extent.width),
			                        .viewportHeight = static_cast<float>(extent.height),
			                        .ambient = frame.ambient,
			                };
			                ctx.recorder.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                ctx.recorder.Draw(6, 1, 0, 0);
		                });
	}
} // namespace aether
