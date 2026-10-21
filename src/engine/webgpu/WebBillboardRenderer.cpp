#include "WebBillboardRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <glm/glm.hpp>

#include "rendering/RenderFramePacket.hpp"
#include "gpu/BillboardPass.hpp"

namespace aether::webgpu
{
	namespace
	{
		WGPUBuffer Upload(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* data, std::size_t size)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = size;
			descriptor.usage = usage | WGPUBufferUsage_CopyDst;
			const auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
			wgpuQueueWriteBuffer(queue, buffer, 0, data, size);
			return buffer;
		}
	} // namespace

	WebBillboardFrame::~WebBillboardFrame()
	{
		for (auto group: bindings)
		{
			wgpuBindGroupRelease(group);
		}
		for (auto buffer: buffers)
		{
			wgpuBufferDestroy(buffer);
			wgpuBufferRelease(buffer);
		}
	}

	WebBillboardRenderer::~WebBillboardRenderer()
	{
		Shutdown();
	}

	void WebBillboardRenderer::Shutdown()
	{
		for (auto& pipeline: m_pipelines)
		{
			if (pipeline)
			{
				wgpuRenderPipelineRelease(pipeline);
				pipeline = nullptr;
			}
		}
		if (m_frame_layout)
		{
			wgpuBindGroupLayoutRelease(m_frame_layout);
			m_frame_layout = nullptr;
		}
		if (m_texture_layout)
		{
			wgpuBindGroupLayoutRelease(m_texture_layout);
			m_texture_layout = nullptr;
		}
		if (m_sampler)
		{
			wgpuSamplerRelease(m_sampler);
			m_sampler = nullptr;
		}
		if (m_white)
		{
			wgpuTextureDestroy(m_white);
			wgpuTextureRelease(m_white);
			m_white = nullptr;
		}
	}

	bool WebBillboardRenderer::Initialize(WGPUDevice device)
	{
		std::ifstream input("/assets/billboard.wgsl");
		const std::string code(std::istreambuf_iterator<char>(input), {});
		if (code.empty())
		{
			error = "Missing billboard WGSL";
			return false;
		}
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {code.data(), code.size()};
		WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shaderDescriptor.nextInChain = &source.chain;
		const auto shader = wgpuDeviceCreateShaderModule(device, &shaderDescriptor);
		std::array<WGPUBindGroupLayoutEntry, 3> entries{};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[0].buffer.type = WGPUBufferBindingType_Uniform;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Vertex;
		entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Fragment;
		entries[2].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
		entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
		WGPUBindGroupLayoutDescriptor groupDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		groupDescriptor.entryCount = entries.size();
		groupDescriptor.entries = entries.data();
		m_frame_layout = wgpuDeviceCreateBindGroupLayout(device, &groupDescriptor);
		entries = {};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Fragment;
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Fragment;
		entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
		groupDescriptor.entryCount = 2;
		m_texture_layout = wgpuDeviceCreateBindGroupLayout(device, &groupDescriptor);
		const std::array<WGPUBindGroupLayout, 2> layouts{m_frame_layout, m_texture_layout};
		WGPUPipelineLayoutDescriptor layoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.bindGroupLayoutCount = layouts.size();
		layoutDescriptor.bindGroupLayouts = layouts.data();
		const auto layout = wgpuDeviceCreatePipelineLayout(device, &layoutDescriptor);
		WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
		WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
		blend.color.operation = blend.alpha.operation = WGPUBlendOperation_Add;
		blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		target.blend = &blend;
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
		fragment.module = shader;
		fragment.entryPoint = {"fragmentMain", 12};
		fragment.targetCount = 1;
		fragment.targets = &target;
		WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
		descriptor.layout = layout;
		descriptor.vertex.module = shader;
		descriptor.vertex.entryPoint = {"vertexMain", 10};
		descriptor.fragment = &fragment;
		descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		descriptor.primitive.cullMode = WGPUCullMode_None;
		for (unsigned display = 0; display < 2; ++display)
		{
			target.format = display ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_RGBA16Float;
			for (unsigned additive = 0; additive < 2; ++additive)
			{
				blend.color.dstFactor = additive ? WGPUBlendFactor_One : WGPUBlendFactor_OneMinusSrcAlpha;
				m_pipelines[display * 2 + additive] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
			}
		}
		wgpuPipelineLayoutRelease(layout);
		wgpuShaderModuleRelease(shader);
		WGPUSamplerDescriptor sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
		sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_Repeat;
		sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
		sampler.mipmapFilter = WGPUMipmapFilterMode_Linear;
		m_sampler = wgpuDeviceCreateSampler(device, &sampler);
		WGPUTextureDescriptor white = WGPU_TEXTURE_DESCRIPTOR_INIT;
		white.size = {1, 1, 1};
		white.dimension = WGPUTextureDimension_2D;
		white.format = WGPUTextureFormat_RGBA8Unorm;
		white.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		m_white = wgpuDeviceCreateTexture(device, &white);
		const std::array<std::uint8_t, 4> pixel{255, 255, 255, 255};
		WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		destination.texture = m_white;
		WGPUTexelCopyBufferLayout copy = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		copy.bytesPerRow = 4;
		copy.rowsPerImage = 1;
		const auto queue = wgpuDeviceGetQueue(device);
		wgpuQueueWriteTexture(queue, &destination, pixel.data(), pixel.size(), &copy, &white.size);
		wgpuQueueRelease(queue);
		return m_sampler && m_white && std::all_of(m_pipelines.begin(), m_pipelines.end(), [](auto value) { return value != nullptr; });
	}

	std::expected<void, std::string> WebBillboardRenderer::Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures) const
	{
		const auto plan = gpu::PlanBillboardPass(packet.render2D.billboards, packet.view, textures.size());
		if (!plan)
		{
			return std::unexpected(plan.error());
		}
		for (const auto& particle: packet.render2D.billboards)
		{
			if (particle.textureIndex != UINT32_MAX && !textures[particle.textureIndex])
			{
				return std::unexpected("Billboard particle references missing Web texture");
			}
		}
		return {};
	}

	void WebBillboardRenderer::Encode(WGPUDevice device,
	        WGPUQueue queue,
	        WGPUCommandEncoder encoder,
	        WGPUTextureView color,
	        unsigned width,
	        unsigned height,
	        const RenderFramePacket& packet,
	        std::span<const WGPUTexture> textures,
	        WGPUTextureView sceneDepth,
	        std::unique_ptr<WebBillboardFrame>& frame,
	        bool displaySpace)
	{
		if (packet.render2D.billboards.empty())
		{
			return;
		}
		if (!frame)
		{
			frame = std::make_unique<WebBillboardFrame>();
		}
		if (!frame->prepared)
		{
			auto plan = gpu::PlanBillboardPass(packet.render2D.billboards, packet.view, textures.size());
			if (!plan)
			{
				error = plan.error();
				return;
			}
			frame->particles = std::move(*plan);
			frame->prepared = true;
		}
		std::vector<BillboardParticleInstance> particles;
		for (const auto& particle: frame->particles)
		{
			if (((particle.blendMode & kBillboardDisplaySpace) != 0) == displaySpace)
			{
				particles.push_back(particle);
			}
		}
		if (particles.empty())
		{
			return;
		}

		struct Constants
		{
			glm::mat4 view, proj, inverseClip;
			glm::vec4 camera, sunDirectionIntensity, sunColor, zenith, extent;
		};

		const Constants constants{glm::transpose(packet.view),
		        glm::transpose(packet.proj),
		        glm::transpose(glm::inverse(packet.proj * packet.view)),
		        packet.cameraWorldPos,
		        packet.sunDirectionIntensity,
		        packet.sunColor,
		        packet.skyZenithColor,
		        glm::vec4(width, height, sceneDepth ? 1 : 0, 0)};
		const auto uniform = Upload(device, queue, WGPUBufferUsage_Uniform, &constants, sizeof(constants));
		const auto instances = Upload(device, queue, WGPUBufferUsage_Storage, particles.data(), particles.size() * sizeof(BillboardParticleInstance));
		frame->buffers.push_back(uniform);
		frame->buffers.push_back(instances);
		const auto fallback = wgpuTextureCreateView(m_white, nullptr);
		std::array<WGPUBindGroupEntry, 3> entries{};
		entries[0].binding = 0;
		entries[0].buffer = uniform;
		entries[0].size = sizeof(constants);
		entries[1].binding = 1;
		entries[1].buffer = instances;
		entries[1].size = particles.size() * sizeof(BillboardParticleInstance);
		entries[2].binding = 2;
		entries[2].textureView = sceneDepth ? sceneDepth : fallback;
		WGPUBindGroupDescriptor binding = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		binding.layout = m_frame_layout;
		binding.entryCount = entries.size();
		binding.entries = entries.data();
		const auto group = wgpuDeviceCreateBindGroup(device, &binding);
		frame->bindings.push_back(group);
		WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		attachment.view = color;
		attachment.loadOp = WGPULoadOp_Load;
		attachment.storeOp = WGPUStoreOp_Store;
		WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		passDescriptor.colorAttachmentCount = 1;
		passDescriptor.colorAttachments = &attachment;
		const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
		for (std::size_t first = 0; first < particles.size();)
		{
			std::size_t end = first + 1;
			while (end < particles.size() && particles[end].textureIndex == particles[first].textureIndex && particles[end].blendMode == particles[first].blendMode)
			{
				++end;
			}
			const auto slot = particles[first].textureIndex;
			if (!frame->textureBindings.contains(slot))
			{
				const auto texture = slot == UINT32_MAX ? m_white : textures[slot];
				const auto view = wgpuTextureCreateView(texture, nullptr);
				std::array<WGPUBindGroupEntry, 2> material{};
				material[0].binding = 0;
				material[0].textureView = view;
				material[1].binding = 1;
				material[1].sampler = m_sampler;
				binding.layout = m_texture_layout;
				binding.entryCount = material.size();
				binding.entries = material.data();
				const auto textureGroup = wgpuDeviceCreateBindGroup(device, &binding);
				frame->bindings.push_back(textureGroup);
				frame->textureBindings.emplace(slot, textureGroup);
				wgpuTextureViewRelease(view);
			}
			const auto textureGroup = frame->textureBindings.at(slot);
			wgpuRenderPassEncoderSetPipeline(pass, m_pipelines[(displaySpace ? 2 : 0) + ((particles[first].blendMode & kBillboardAdditive) ? 1 : 0)]);
			wgpuRenderPassEncoderSetBindGroup(pass, 1, textureGroup, 0, nullptr);
			wgpuRenderPassEncoderDraw(pass, 6, end - first, 0, first);
			first = end;
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
		wgpuTextureViewRelease(fallback);
	}
} // namespace aether::webgpu
