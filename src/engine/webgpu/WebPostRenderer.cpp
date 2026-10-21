#include "WebPostRenderer.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	namespace
	{
		std::unique_ptr<WebEffectImage> Image(WGPUDevice device, unsigned width, unsigned height, WGPUTextureFormat format)
		{
			auto image = std::make_unique<WebEffectImage>();
			image->width = width;
			image->height = height;
			WGPUTextureDescriptor descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
			descriptor.size = {width, height, 1};
			descriptor.dimension = WGPUTextureDimension_2D;
			descriptor.format = format;
			descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
			image->texture = wgpuDeviceCreateTexture(device, &descriptor);
			if (!image->texture)
			{
				return {};
			}
			image->view = wgpuTextureCreateView(image->texture, nullptr);
			if (!image->view)
			{
				return {};
			}
			return image;
		}

		struct BackgroundUniform
		{
			std::uint32_t mode, count;
			float angle;
			std::uint32_t pad = 0;
			std::array<glm::vec4, 8> stops{};
		};

		struct TonemapUniform
		{
			std::uint32_t hdrSlot = 0, mode = 1;
			float exposure = 1;
			std::uint32_t debugCompare = 0, debugModeCount = 0;
			std::int32_t inspectX = -1, inspectY = -1;
			std::uint32_t width = 0, height = 0, bloomSlot = UINT32_MAX;
			float bloomStrength = 0;
			std::uint32_t dofSlot = UINT32_MAX, background = 0;
			float contrast = 1, saturation = 1, temperature = 0, tint = 0, vignette = 0, roundness = 1;
			std::uint32_t depthSlot = UINT32_MAX;
			float motion = 0, motionRadius = 64;
			std::uint32_t pad = 0, frameAddress = 1;
			float aberration = 0, grain = 0;
			std::int32_t originX = 0, originY = 0;
		};

		static_assert(sizeof(TonemapUniform) == 112);
	} // namespace

	struct WebPostRenderer::ShaderFrame
	{
		glm::mat4 view{1}, proj{1}, viewProj{1}, invViewProj{1}, prevViewProj{1};
		std::array<glm::mat4, 3> shadowViewProjCascades{};
		glm::vec4 cameraWorldPos{}, sunDirectionIntensity{}, sunColor{};
		glm::vec4 skyHorizonColor{}, skyZenithColor{}, skyVoidColor{};
		glm::vec4 fogParams{}, skyParams{}, shadingParams{}, shadowCascadeSplits{}, shadowCascadeDepthBias{};
		std::array<glm::vec4, 3> shadowRegions{};
		std::array<glm::vec4, 8> resourceSizes{};
		float elapsedTime = 0;
		std::uint32_t resourceTableAddr = 0, pad0 = 0, pad1 = 0;
	};

	WebEffectImage::~WebEffectImage()
	{
		if (view)
		{
			wgpuTextureViewRelease(view);
		}
		if (texture)
		{
			wgpuTextureDestroy(texture);
			wgpuTextureRelease(texture);
		}
	}

	WebPostFrame::~WebPostFrame()
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

	WebOpaqueInputs WebPostFrame::OpaqueInputs() const
	{
		WebOpaqueInputs result;
		result.sceneDepth = depth->view;
		result.normalRoughness = normal->view;
		result.albedo = albedo->view;
		result.depthAttachment = depthAttachment->view;
		result.ambientOcclusion = aoDenoised ? aoDenoised->view : nullptr;
		return result;
	}

	WebPostRenderer::~WebPostRenderer()
	{
		Shutdown();
	}

	void WebPostRenderer::Shutdown()
	{
		for (auto& pipeline: m_pipelines)
		{
			if (pipeline)
			{
				wgpuRenderPipelineRelease(pipeline);
				pipeline = nullptr;
			}
		}
		if (m_layout)
		{
			wgpuBindGroupLayoutRelease(m_layout);
			m_layout = nullptr;
		}
		if (m_sampler)
		{
			wgpuSamplerRelease(m_sampler);
			m_sampler = nullptr;
		}
		m_white.reset();
		m_exposure.Shutdown();
		m_previous_width = m_previous_height = 0;
		m_previous_time = -1;
	}

	bool WebPostRenderer::Initialize(WGPUDevice device, WGPUQueue queue)
	{
		Shutdown();
		static_assert(sizeof(ShaderFrame) == 880);
		std::array<WGPUBindGroupLayoutEntry, 13> entries{};
		for (unsigned index = 0; index < entries.size(); ++index)
		{
			auto& entry = entries[index];
			entry.binding = index;
			entry.visibility = WGPUShaderStage_Fragment;
			if (index == 12)
			{
				entry.binding = 13;
				entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
				entry.buffer.minBindingSize = 16;
			}
			else if (index < 2 || index == 11)
			{
				entry.buffer.type = WGPUBufferBindingType_Uniform;
				entry.buffer.minBindingSize = index == 0 ? sizeof(ShaderFrame) : index == 1 ? 256 : sizeof(BackgroundUniform);
			}
			else if (index == 10)
			{
				entry.sampler.type = WGPUSamplerBindingType_Filtering;
			}
			else
			{
				entry.texture.sampleType = index == 3 || index == 9 ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
				entry.texture.viewDimension = WGPUTextureViewDimension_2D;
			}
		}
		WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.entryCount = entries.size();
		layoutDescriptor.entries = entries.data();
		m_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
		pipelineLayoutDescriptor.bindGroupLayouts = &m_layout;
		const auto layout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
		const std::array names{"bloom", "bloom", "fxaa", "tonemap", "gtao_main", "gtao_denoise", "ssr", "ssr_composite", "dof", "volumetric_fog", "volumetric_composite"};
		for (unsigned index = 0; index < names.size(); ++index)
		{
			std::ifstream file(std::string("/assets/post-") + names[index] + ".wgsl");
			const std::string code(std::istreambuf_iterator<char>(file), {});
			if (code.empty())
			{
				error = "WEB_EFFECT_SHADER: missing post-" + std::string(names[index]) + ".wgsl";
				break;
			}
			WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
			source.code = {code.data(), code.size()};
			WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
			moduleDescriptor.nextInChain = &source.chain;
			const auto module = wgpuDeviceCreateShaderModule(device, &moduleDescriptor);
			WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
			color.format = index == 2 || index == 3 ? WGPUTextureFormat_RGBA8Unorm : index == 4 || index == 5 ? WGPUTextureFormat_R8Unorm : WGPUTextureFormat_RGBA16Float;
			WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
			if (index == 1 || index == 7 || index == 10)
			{
				blend.color.srcFactor = blend.alpha.srcFactor = WGPUBlendFactor_One;
				blend.color.dstFactor = index == 10 ? WGPUBlendFactor_OneMinusSrcAlpha : WGPUBlendFactor_One;
				blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
				color.blend = &blend;
			}
			WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
			fragment.module = module;
			const char* entry = index == 0 ? "downsampleMain" : index == 1 ? "upsampleMain" : "fragmentMain";
			fragment.entryPoint = {entry, std::strlen(entry)};
			fragment.targetCount = 1;
			fragment.targets = &color;
			WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
			descriptor.layout = layout;
			descriptor.vertex.module = module;
			descriptor.vertex.entryPoint = {"vertexMain", 10};
			descriptor.fragment = &fragment;
			descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
			m_pipelines[index] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
			wgpuShaderModuleRelease(module);
		}
		wgpuPipelineLayoutRelease(layout);
		WGPUSamplerDescriptor sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
		sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
		sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
		m_sampler = wgpuDeviceCreateSampler(device, &sampler);
		m_white = Image(device, 1, 1, WGPUTextureFormat_RGBA8Unorm);
		if (m_white)
		{
			const std::uint32_t white = UINT32_MAX;
			WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			destination.texture = m_white->texture;
			WGPUTexelCopyBufferLayout bytes = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
			bytes.bytesPerRow = 4;
			const WGPUExtent3D extent{1, 1, 1};
			wgpuQueueWriteTexture(queue, &destination, &white, 4, &bytes, &extent);
		}
		if (!m_layout || !m_sampler || !m_white || std::ranges::find(m_pipelines, nullptr) != m_pipelines.end() || !m_exposure.Initialize(device))
		{
			if (error.empty())
			{
				error = "WEB_EFFECT_PIPELINE: post-processing initialization failed";
			}
			Shutdown();
			return false;
		}
		return true;
	}

	std::expected<std::unique_ptr<WebPostFrame>, std::string> WebPostRenderer::Allocate(WGPUDevice device, WGPUQueue queue, const gpu::PostEffectsPlan& plan, const RenderFramePacket& packet)
	{
		auto frame = std::make_unique<WebPostFrame>();
		frame->plan = plan;
		auto exposure = m_exposure.Prepare(device, queue, packet, plan.width, plan.height, packet.renderSceneGeneration);
		if (!exposure)
		{
			return std::unexpected(exposure.error());
		}
		frame->exposureState = std::move(*exposure);
		const auto allocate = [&](unsigned width, unsigned height, WGPUTextureFormat format)
		{
			auto image = Image(device, width, height, format);
			auto* pointer = image.get();
			frame->images.push_back(std::move(image));
			return pointer;
		};
		const auto w = plan.width, h = plan.height, halfW = (w + 1) / 2, halfH = (h + 1) / 2;
		frame->depth = allocate(w, h, WGPUTextureFormat_R32Float);
		frame->normal = allocate(w, h, WGPUTextureFormat_RGBA8Unorm);
		frame->albedo = allocate(w, h, WGPUTextureFormat_RGBA8UnormSrgb);
		frame->depthAttachment = allocate(w, h, WGPUTextureFormat_Depth32Float);
		frame->ldr = allocate(w, h, WGPUTextureFormat_RGBA8Unorm);
		if (packet.postEffects.renderScale != 1)
		{
			frame->resolved = allocate(w, h, WGPUTextureFormat_RGBA8Unorm);
		}
		if (packet.postEffects.gtao)
		{
			frame->ao = allocate(halfW, halfH, WGPUTextureFormat_R8Unorm);
			frame->aoDenoised = allocate(halfW, halfH, WGPUTextureFormat_R8Unorm);
		}
		if (packet.postEffects.reflections)
		{
			frame->reflection = allocate(w, h, WGPUTextureFormat_RGBA16Float);
		}
		if (packet.postEffects.volumetrics && packet.skyParams.z > 0 && packet.fogParams.x > 0)
		{
			frame->fog = allocate(halfW, halfH, WGPUTextureFormat_RGBA16Float);
		}
		if (plan.depthOfField)
		{
			frame->dof = allocate(halfW, halfH, WGPUTextureFormat_RGBA16Float);
		}
		if (packet.postEffects.bloomStrength > 0)
		{
			unsigned width = w, height = h;
			for (auto& mip: frame->bloom)
			{
				width = std::max(1u, width / 2);
				height = std::max(1u, height / 2);
				mip = allocate(width, height, WGPUTextureFormat_RGBA16Float);
			}
		}
		if (std::ranges::any_of(frame->images, [](const auto& image) { return !image; }))
		{
			return std::unexpected("WEB_EFFECT_ALLOCATE: could not allocate bounded effect targets");
		}
		return frame;
	}

	WebPostRenderer::ShaderFrame WebPostRenderer::FrameConstants(const RenderFramePacket& packet, const WebPostFrame& frame, const WebShadowInputs& shadows) const
	{
		ShaderFrame result;
		result.view = packet.view;
		result.proj = packet.proj;
		result.viewProj = packet.proj * packet.view;
		result.invViewProj = glm::inverse(result.viewProj);
		const bool reset = !m_previous_width || m_previous_width != frame.plan.width || m_previous_height != frame.plan.height;
		result.prevViewProj = reset ? result.viewProj : m_previous;
		result.shadowViewProjCascades = shadows.viewProj;
		result.cameraWorldPos = packet.cameraWorldPos;
		result.sunDirectionIntensity = packet.sunDirectionIntensity;
		result.sunColor = packet.sunColor;
		result.skyHorizonColor = packet.skyHorizonColor;
		result.skyZenithColor = packet.skyZenithColor;
		result.skyVoidColor = packet.skyVoidColor;
		result.fogParams = packet.fogParams;
		result.skyParams = packet.skyParams;
		result.shadingParams = packet.shadingParams;
		result.shadowCascadeSplits = shadows.splits;
		result.shadowCascadeDepthBias = shadows.depthBias;
		result.shadowRegions = shadows.regions;
		if (!shadows.enabled)
		{
			result.shadowRegions = {};
		}
		for (unsigned index = 0; index < 8; ++index)
		{
			result.resourceSizes[index] = glm::vec4(frame.plan.width, frame.plan.height, 1, 0);
		}
		if (frame.aoDenoised)
		{
			result.resourceSizes[4] = glm::vec4(frame.aoDenoised->width, frame.aoDenoised->height, 1, 0);
		}
		else
		{
			result.resourceSizes[4] = glm::vec4(1, 1, 0, 0);
		}
		result.resourceSizes[7] = glm::vec4(4096, 7168, shadows.enabled ? 1 : 0, 0);
		result.elapsedTime = packet.elapsedTime;
		return result;
	}

	void WebPostRenderer::ClearOpaque(WGPUCommandEncoder encoder, const WebPostFrame& frame) const
	{
		std::array<WGPURenderPassColorAttachment, 3> colors{};
		for (auto& color: colors)
		{
			color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			color.loadOp = WGPULoadOp_Clear;
			color.storeOp = WGPUStoreOp_Store;
		}
		colors[0].view = frame.depth->view;
		colors[0].clearValue = {1, 0, 0, 0};
		colors[1].view = frame.normal->view;
		colors[2].view = frame.albedo->view;
		WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		depth.view = frame.depthAttachment->view;
		depth.depthLoadOp = WGPULoadOp_Clear;
		depth.depthStoreOp = WGPUStoreOp_Store;
		depth.depthClearValue = 1;
		WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		descriptor.colorAttachmentCount = colors.size();
		descriptor.colorAttachments = colors.data();
		descriptor.depthStencilAttachment = &depth;
		const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}

	void WebPostRenderer::Draw(WGPUDevice device,
	        WGPUQueue queue,
	        WGPUCommandEncoder encoder,
	        Pass pass,
	        WGPUTextureView target,
	        unsigned width,
	        unsigned height,
	        const std::array<WGPUTextureView, 8>& textures,
	        const ShaderFrame& constants,
	        const void* push,
	        std::size_t pushSize,
	        const RenderFramePacket& packet,
	        WebPostFrame& frame,
	        bool load)
	{
		const auto buffer = [&](const void* data, std::size_t size)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = size;
			descriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
			const auto result = wgpuDeviceCreateBuffer(device, &descriptor);
			wgpuQueueWriteBuffer(queue, result, 0, data, size);
			frame.buffers.push_back(result);
			return result;
		};
		std::array<std::byte, 256> pushBytes{};
		std::memcpy(pushBytes.data(), push, pushSize);
		const BackgroundUniform background{packet.backgroundMode, packet.backgroundStopCount, packet.backgroundAngleRadians, 0, packet.backgroundStops};
		std::array<WGPUBindGroupEntry, 13> entries{};
		for (unsigned i = 0; i < entries.size(); ++i)
		{
			entries[i].binding = i;
		}
		entries[0].buffer = buffer(&constants, sizeof(constants));
		entries[0].size = sizeof(constants);
		entries[1].buffer = buffer(pushBytes.data(), pushBytes.size());
		entries[1].size = pushBytes.size();
		for (unsigned i = 0; i < textures.size(); ++i)
		{
			// An effect never binds its writable attachment as a sampled resource.
			entries[i + 2].textureView = textures[i] && textures[i] != target ? textures[i] : m_white->view;
		}
		entries[10].sampler = m_sampler;
		entries[11].buffer = buffer(&background, sizeof(background));
		entries[11].size = sizeof(background);
		entries[12].binding = 13;
		entries[12].buffer = frame.exposureState->buffer;
		entries[12].size = 16;
		WGPUBindGroupDescriptor group = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		group.layout = m_layout;
		group.entryCount = entries.size();
		group.entries = entries.data();
		const auto bindings = wgpuDeviceCreateBindGroup(device, &group);
		frame.bindings.push_back(bindings);
		WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		color.view = target;
		color.loadOp = load ? WGPULoadOp_Load : WGPULoadOp_Clear;
		color.storeOp = WGPUStoreOp_Store;
		WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		descriptor.colorAttachmentCount = 1;
		descriptor.colorAttachments = &color;
		const auto commands = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
		wgpuRenderPassEncoderSetPipeline(commands, m_pipelines[static_cast<unsigned>(pass)]);
		wgpuRenderPassEncoderSetBindGroup(commands, 0, bindings, 0, nullptr);
		wgpuRenderPassEncoderDraw(commands, 3, 1, 0, 0);
		wgpuRenderPassEncoderEnd(commands);
		wgpuRenderPassEncoderRelease(commands);
	}

	void WebPostRenderer::EncodeAmbient(WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, const RenderFramePacket& packet, WebPostFrame& frame, const WebShadowInputs& shadows)
	{
		if (!frame.ao)
		{
			return;
		}
		const auto constants = FrameConstants(packet, frame, shadows);
		std::array<WGPUTextureView, 8> inputs{nullptr, frame.depth->view, frame.normal->view, frame.albedo->view};

		struct GtaoPush
		{
			unsigned width, height, frameIndex, address;
			float radius, strength;
			unsigned enabled, pad;
		};

		const auto temporalFrame = packet.temporalFrameIndex == UINT64_MAX ? packet.frameIndex : packet.temporalFrameIndex;
		const GtaoPush gtao{frame.plan.width, frame.plan.height, static_cast<unsigned>(temporalFrame), 1, packet.postEffects.gtaoRadius, packet.postEffects.gtaoStrength, 1, 0};
		Draw(device, queue, encoder, Pass::Gtao, frame.ao->view, frame.ao->width, frame.ao->height, inputs, constants, &gtao, sizeof(gtao), packet, frame);
		inputs[5] = frame.ao->view;

		struct DenoisePush
		{
			unsigned slot, address, width, height, aoWidth, aoHeight;
			float threshold, sigma;
		};

		const DenoisePush denoise{5, 1, frame.plan.width, frame.plan.height, frame.ao->width, frame.ao->height, .0012f, 1.6f};
		Draw(device, queue, encoder, Pass::Denoise, frame.aoDenoised->view, frame.aoDenoised->width, frame.aoDenoised->height, inputs, constants, &denoise, sizeof(denoise), packet, frame);
	}

	void WebPostRenderer::EncodeScene(WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, WGPUTextureView scene, const RenderFramePacket& packet, WebPostFrame& frame, const WebShadowInputs& shadows)
	{
		const auto constants = FrameConstants(packet, frame, shadows);
		const auto& settings = packet.postEffects;
		const unsigned w = frame.plan.width, h = frame.plan.height;
		std::array<WGPUTextureView, 8> inputs{scene, frame.depth->view, frame.normal->view, frame.albedo->view, frame.aoDenoised ? frame.aoDenoised->view : nullptr, nullptr, nullptr, shadows.atlas};
		if (frame.reflection)
		{
			struct Push
			{
				unsigned address, width, height;
				float roughness, intensity;
				unsigned pad0, pad1;
			};

			const Push push{1, w, h, settings.reflectionMaxRoughness, settings.reflectionIntensity, 0, 0};
			Draw(device, queue, encoder, Pass::Reflection, frame.reflection->view, w, h, inputs, constants, &push, sizeof(push), packet, frame);
			inputs[5] = frame.reflection->view;
			const std::array<unsigned, 5> composite{1, 5, w, h, 0};
			Draw(device, queue, encoder, Pass::ReflectionComposite, scene, w, h, inputs, constants, composite.data(), sizeof(composite), packet, frame, true);
		}
		if (frame.fog)
		{
			const std::array<unsigned, 5> push{1, frame.fog->width, frame.fog->height, 1, 0};
			Draw(device, queue, encoder, Pass::Fog, frame.fog->view, frame.fog->width, frame.fog->height, inputs, constants, push.data(), sizeof(push), packet, frame);
			inputs[5] = frame.fog->view;
			const std::array<unsigned, 7> composite{1, 5, 1, frame.fog->width, frame.fog->height, 0, 0};
			Draw(device, queue, encoder, Pass::FogComposite, scene, w, h, inputs, constants, composite.data(), sizeof(composite), packet, frame, true);
		}
		if (frame.dof)
		{
			struct Push
			{
				unsigned address, hdr, depth, width, height;
				float focus, coefficient, maxCoC;
			};

			const Push push{1, 0, 1, frame.dof->width, frame.dof->height, packet.dofParams.x, packet.dofParams.y, packet.dofParams.z};
			Draw(device, queue, encoder, Pass::Dof, frame.dof->view, frame.dof->width, frame.dof->height, inputs, constants, &push, sizeof(push), packet, frame);
		}
	}

	void WebPostRenderer::EncodeDisplay(WGPUDevice device,
	        WGPUQueue queue,
	        WGPUCommandEncoder encoder,
	        WGPUTextureView scene,
	        WGPUTextureView canvas,
	        unsigned outputWidth,
	        unsigned outputHeight,
	        const RenderFramePacket& packet,
	        WebPostFrame& frame,
	        const WebShadowInputs& shadows,
	        const std::function<void(WGPUTextureView, unsigned, unsigned)>& afterTonemap)
	{
		auto constants = FrameConstants(packet, frame, shadows);
		const auto& settings = packet.postEffects;
		const unsigned w = frame.plan.width, h = frame.plan.height;
		std::array<WGPUTextureView, 8> inputs{scene, frame.depth->view, frame.normal->view, frame.albedo->view, frame.aoDenoised ? frame.aoDenoised->view : nullptr, nullptr, nullptr, shadows.atlas};
		if (frame.bloom[0])
		{
			unsigned sourceWidth = w, sourceHeight = h;
			for (unsigned i = 0; i < frame.bloom.size(); ++i)
			{
				struct Push
				{
					unsigned slot;
					float width, height, radius, karis;
				};

				const Push push{0, float(sourceWidth), float(sourceHeight), settings.bloomRadius, i == 0 ? 1.0f : 0.0f};
				Draw(device, queue, encoder, Pass::BloomDown, frame.bloom[i]->view, frame.bloom[i]->width, frame.bloom[i]->height, inputs, constants, &push, sizeof(push), packet, frame);
				inputs[0] = frame.bloom[i]->view;
				sourceWidth = frame.bloom[i]->width;
				sourceHeight = frame.bloom[i]->height;
			}
			for (unsigned i = 4; i > 0; --i)
			{
				struct Push
				{
					unsigned slot;
					float width, height, radius, karis;
				};

				const Push push{0, float(frame.bloom[i]->width), float(frame.bloom[i]->height), settings.bloomRadius, 0};
				inputs[0] = frame.bloom[i]->view;
				Draw(device, queue, encoder, Pass::BloomUp, frame.bloom[i - 1]->view, frame.bloom[i - 1]->width, frame.bloom[i - 1]->height, inputs, constants, &push, sizeof(push), packet, frame, true);
			}
		}
		inputs[0] = scene;
		inputs[5] = frame.dof ? frame.dof->view : nullptr;
		inputs[6] = frame.bloom[0] ? frame.bloom[0]->view : nullptr;
		if (packet.advanceTemporalEffects)
		{
			m_exposure.EncodeAdapt(device, queue, encoder, scene, w, h, settings, frame.exposureState, frame.exposureFrame);
		}
		TonemapUniform tone;
		tone.mode = packet.tonemapMode;
		tone.exposure = packet.tonemapExposure;
		tone.width = w;
		tone.height = h;
		tone.bloomSlot = frame.bloom[0] ? 6 : UINT32_MAX;
		tone.bloomStrength = settings.bloomStrength;
		tone.dofSlot = frame.dof ? 5 : UINT32_MAX;
		tone.background = packet.backgroundMode == 2 ? 0 : 1;
		tone.contrast = settings.gradeContrast;
		tone.saturation = settings.gradeSaturation;
		tone.temperature = settings.gradeTemperature;
		tone.tint = settings.gradeTint;
		tone.vignette = settings.vignetteIntensity;
		tone.roundness = settings.vignetteRoundness;
		tone.depthSlot = 1;
		tone.motion = settings.motionBlur;
		tone.motionRadius = settings.motionBlurMaxRadius;
		tone.pad = settings.autoExposure ? 1u : 0u;
		tone.aberration = settings.chromaticAberration;
		tone.grain = settings.filmGrain;
		const bool resolve = settings.fxaa || settings.sharpness > 0 || w != outputWidth || h != outputHeight;
		Draw(device, queue, encoder, Pass::Tonemap, resolve ? frame.ldr->view : canvas, w, h, inputs, constants, &tone, sizeof(tone), packet, frame);
		if (afterTonemap)
		{
			afterTonemap(resolve ? frame.ldr->view : canvas, w, h);
		}
		if (packet.advanceTemporalEffects)
		{
			m_exposure.Encode(device, queue, encoder, scene, w, h, settings, frame.exposureState, frame.exposureFrame);
		}
		if (resolve)
		{
			inputs[0] = frame.ldr->view;

			struct Push
			{
				unsigned slot, enabled;
				float rcpWidth, rcpHeight, sharpness, outputRcpWidth, outputRcpHeight;
			};

			const bool scale = w != outputWidth || h != outputHeight;
			const Push push{0, settings.fxaa ? 1u : 0u, 1.0f / w, 1.0f / h, settings.sharpness, 1.0f / w, 1.0f / h};
			Draw(device, queue, encoder, Pass::Fxaa, scale ? frame.resolved->view : canvas, w, h, inputs, constants, &push, sizeof(push), packet, frame);
			if (scale)
			{
				inputs[0] = frame.resolved->view;
				const Push present{0, 2, 1.0f / w, 1.0f / h, 0, 1.0f / outputWidth, 1.0f / outputHeight};
				Draw(device, queue, encoder, Pass::Fxaa, canvas, outputWidth, outputHeight, inputs, constants, &present, sizeof(present), packet, frame);
			}
		}
		if (packet.advanceTemporalEffects)
		{
			m_previous = constants.viewProj;
			m_previous_width = w;
			m_previous_height = h;
			m_previous_time = packet.elapsedTime;
		}
	}
} // namespace aether::webgpu
