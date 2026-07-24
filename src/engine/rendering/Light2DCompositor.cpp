#include "rendering/Light2DCompositor.hpp"

#include <algorithm>
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
		constexpr float kLightClampCeiling = 4.0f; // multiply ceiling: over-bright stacks saturate

		// Shadow tuning: march resolution and how far (world units) to skip near the shading point so a
		// lit wall face doesn't shadow itself. Strength/softness come per-scene from the packet.
		constexpr float kShadowSteps = 16.0f;
		constexpr float kShadowWorldBias = 0.9f;

		// MUST match Light2DPush in shaders/light2d.slang (64 bytes).
		struct Light2DPush
		{
			gpu::DeviceAddress frameConstants;
			gpu::DeviceAddress lights;
			std::uint32_t count;
			std::uint32_t occluderSlot;
			float viewportWidth;
			float viewportHeight;
			glm::vec4 ambient;
			glm::vec4 shadowParams;
		};

		// MUST match OccluderPush in shaders/occluder2d.slang (24 bytes).
		struct OccluderPush
		{
			gpu::DeviceAddress frameConstants;
			gpu::DeviceAddress occluders;
			std::uint32_t count;
			std::uint32_t pad;
		};

		static_assert(sizeof(Light2DPush) == 64);
		static_assert(sizeof(OccluderPush) == 24);
		static_assert(sizeof(GpuLight2D) == 64);
		static_assert(sizeof(Occluder2D) == 48);
	} // namespace

	void Light2DCompositor::Initialize(GpuDevice& gpu, gpu::Format colorFormat)
	{
		AE_PROFILE_ZONE();
		const gpu::GraphicsPipelineDesc composite{
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
		m_pipeline = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), composite);
		if (!m_pipeline.IsValid())
		{
			AE_ERROR(LogCategory::Render, "Light2DCompositor: failed to create composite pipeline");
		}

		const gpu::GraphicsPipelineDesc occluder{
		        .shaderVfsPath = "shaders://occluder2d.spv",
		        .colorFormat = gpu::Format::R8Unorm,
		        .depthFormat = gpu::Format::Undefined,
		        .depthTestEnable = false,
		        .depthWriteEnable = false,
		        .blendEnable = false,
		        .blendMode = gpu::BlendMode::Opaque,
		        .topology = gpu::PrimitiveTopology::TriangleList,
		        .polygonMode = gpu::PolygonMode::Fill,
		        .cullMode = gpu::CullMode::None,
		        .debugName = "Light2DOccluder",
		        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
		};
		m_occluderPipeline = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), occluder);
		if (!m_occluderPipeline.IsValid())
		{
			AE_ERROR(LogCategory::Render, "Light2DCompositor: failed to create occluder pipeline");
		}
	}

	void Light2DCompositor::Shutdown()
	{
		AE_PROFILE_ZONE();
		for (Frame& frame: m_frames)
		{
			if (frame.lights.buffer.IsValid())
			{
				gpu::ResourceRegistry::Destroy(frame.lights.buffer);
			}
			if (frame.occluders.buffer.IsValid())
			{
				gpu::ResourceRegistry::Destroy(frame.occluders.buffer);
			}
			frame = Frame{};
		}
		if (m_pipeline.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_pipeline);
			m_pipeline = {};
		}
		if (m_occluderPipeline.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_occluderPipeline);
			m_occluderPipeline = {};
		}
	}

	void Light2DCompositor::EnsureCapacity(GpuBuffer& buffer, std::uint32_t count, std::size_t stride, const char* debugName)
	{
		if (buffer.capacity >= count && buffer.buffer.IsValid())
		{
			return;
		}
		std::uint32_t capacity = buffer.capacity == 0 ? static_cast<std::uint32_t>(std::max<std::size_t>(1, 4096 / stride)) : buffer.capacity;
		while (capacity < count)
		{
			capacity *= 2;
		}
		if (buffer.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(buffer.buffer);
		}
		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(capacity) * stride,
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = debugName,
		};
		buffer.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!buffer.buffer.IsValid())
		{
			buffer.mapped = nullptr;
			buffer.address = 0;
			buffer.capacity = 0;
			AE_ERROR(LogCategory::Render, "Light2DCompositor: failed to allocate {} records for {}", capacity, debugName);
			return;
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(buffer.buffer);
		buffer.mapped = view.mappedPtr;
		buffer.address = view.deviceAddress;
		buffer.capacity = capacity;
	}

	void Light2DCompositor::BeginFrame(const RenderFramePacket& packet, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();
		Frame& frame = m_frames[frameSlot % kFrames];
		frame.lights.count = 0;
		frame.occluders.count = 0;

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
			g.flags = glm::vec4(l.castsShadow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
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
			g.flags = glm::vec4(l.castsShadow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
			lights.push_back(g);
		}

		const auto lightCount = static_cast<std::uint32_t>(lights.size());
		EnsureCapacity(frame.lights, lightCount, sizeof(GpuLight2D), "Light2D.Lights");
		if (!frame.lights.buffer.IsValid())
		{
			return;
		}
		std::memcpy(frame.lights.mapped, lights.data(), static_cast<std::size_t>(lightCount) * sizeof(GpuLight2D));
		gpu::ResourceRegistry::FlushMappedBuffer(frame.lights.buffer, 0, static_cast<gpu::DeviceSize>(lightCount) * sizeof(GpuLight2D));
		frame.lights.count = lightCount;
		frame.ambient = glm::vec4(glm::vec3(packet.light2DAmbient), kLightClampCeiling);
		frame.shadowParams = packet.light2DShadowParams; // x = strength, y = softness

		// Shadow occluders: solid tile cells emitted by the tilemap system this frame.
		const auto occluderCount = static_cast<std::uint32_t>(packet.render2D.occluders.size());
		if (occluderCount > 0)
		{
			EnsureCapacity(frame.occluders, occluderCount, sizeof(Occluder2D), "Light2D.Occluders");
			if (frame.occluders.buffer.IsValid())
			{
				const auto bytes = static_cast<gpu::DeviceSize>(occluderCount) * sizeof(Occluder2D);
				std::memcpy(frame.occluders.mapped, packet.render2D.occluders.data(), bytes);
				gpu::ResourceRegistry::FlushMappedBuffer(frame.occluders.buffer, 0, bytes);
				frame.occluders.count = occluderCount;
			}
		}
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
		// Graph-managed transient occluder mask (auto-sized, sampled by the composite). R8: 1 = solid.
		const RGImage occluderMask = graph.CreateTransientColor(gpu::Format::R8Unorm, extent, gpu::ImageUsage::Sampled);

		// Pass 1: rasterise solid tile cells into the mask.
		graph.AddFullscreenPass({
		             .name = "$Light2DOccluders",
		             .color = occluderMask,
		             .extent = extent,
		             .loadOp = gpu::LoadOp::Clear,
		     })
		        .Execute(
		                [this, &bindless, frameConstants, enabled](PassContext& ctx)
		                {
			                if (enabled != nullptr && !enabled->load(std::memory_order_relaxed))
			                {
				                return;
			                }
			                Frame& frame = m_frames[ctx.frameSlot % kFrames];
			                if (frame.lights.count == 0 || frame.occluders.count == 0 || !m_occluderPipeline.IsValid())
			                {
				                return; // mask stays cleared to 0 (no shadows)
			                }
			                bindless.CmdBindHeaps(ctx.recorder);
			                ctx.recorder.BindPipeline(gpu::ResourceRegistry::ResolvePipeline(m_occluderPipeline).state);
			                const OccluderPush push{
			                        .frameConstants = frameConstants != nullptr ? frameConstants->GetDeviceAddress(ctx.frameSlot) : ctx.frameConstantsAddr,
			                        .occluders = frame.occluders.address,
			                        .count = frame.occluders.count,
			                        .pad = 0,
			                };
			                ctx.recorder.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                ctx.recorder.Draw(6, frame.occluders.count, 0, 0);
		                });

		const std::uint32_t occluderSlot = graph.EnsureBindlessSampled(occluderMask);

		// Pass 2: multiply the scene HDR colour by (ambient + shadowed lights).
		graph.AddFullscreenPass({
		             .name = std::string(name),
		             .color = color,
		             .extent = extent,
		             .loadOp = gpu::LoadOp::Load,
		     })
		        .ReadTexture(occluderMask)
		        .Execute(
		                [this, &bindless, extent, frameConstants, enabled, occluderSlot](PassContext& ctx)
		                {
			                if (enabled != nullptr && !enabled->load(std::memory_order_relaxed))
			                {
				                return;
			                }
			                Frame& frame = m_frames[ctx.frameSlot % kFrames];
			                if (frame.lights.count == 0 || !m_pipeline.IsValid() || !frame.lights.buffer.IsValid())
			                {
				                return;
			                }
			                bindless.CmdBindHeaps(ctx.recorder);
			                ctx.recorder.BindPipeline(gpu::ResourceRegistry::ResolvePipeline(m_pipeline).state);
			                // The mask drives both directional (normal) shading and cast shadows, so bind it
			                // whenever occluders exist; shadow STRENGTH separately gates the cast-shadow march.
			                const bool haveMask = frame.occluders.count > 0 && occluderSlot != 0xFFFFFFFFu;
			                const bool doShadows = haveMask && frame.shadowParams.x > 0.0f;
			                const Light2DPush push{
			                        .frameConstants = frameConstants != nullptr ? frameConstants->GetDeviceAddress(ctx.frameSlot) : ctx.frameConstantsAddr,
			                        .lights = frame.lights.address,
			                        .count = frame.lights.count,
			                        .occluderSlot = haveMask ? occluderSlot : 0xFFFFFFFFu,
			                        .viewportWidth = static_cast<float>(extent.width),
			                        .viewportHeight = static_cast<float>(extent.height),
			                        .ambient = frame.ambient,
			                        // x = cast-shadows enabled, y = strength, z = softness, w = world bias. Steps are a shader constant.
			                        .shadowParams = glm::vec4(doShadows ? 1.0f : 0.0f, frame.shadowParams.x, frame.shadowParams.y, kShadowWorldBias),
			                };
			                ctx.recorder.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                ctx.recorder.Draw(6, 1, 0, 0);
		                });
	}
} // namespace aether
