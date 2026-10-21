#include "WebHdrRenderer.hpp"

#include <fstream>
#include <iterator>
#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	WebHdrFrame::~WebHdrFrame()
	{
		if (backgroundBindings)
		{
			wgpuBindGroupRelease(backgroundBindings);
		}
		if (compositeBindings)
		{
			wgpuBindGroupRelease(compositeBindings);
		}
		if (uniform)
		{
			wgpuBufferDestroy(uniform);
			wgpuBufferRelease(uniform);
		}
		if (view)
		{
			wgpuTextureViewRelease(view);
		}
		if (color)
		{
			wgpuTextureDestroy(color);
			wgpuTextureRelease(color);
		}
	}

	WebHdrRenderer::~WebHdrRenderer()
	{
		Shutdown();
	}

	void WebHdrRenderer::Shutdown()
	{
		if (m_background)
		{
			wgpuRenderPipelineRelease(m_background);
			m_background = nullptr;
		}
		if (m_composite)
		{
			wgpuRenderPipelineRelease(m_composite);
			m_composite = nullptr;
		}
		if (m_background_layout)
		{
			wgpuBindGroupLayoutRelease(m_background_layout);
			m_background_layout = nullptr;
		}
		if (m_composite_layout)
		{
			wgpuBindGroupLayoutRelease(m_composite_layout);
			m_composite_layout = nullptr;
		}
	}

	bool WebHdrRenderer::Initialize(WGPUDevice device)
	{
		Shutdown();
		std::ifstream input("/assets/hdr.wgsl");
		const std::string code(std::istreambuf_iterator<char>(input), {});
		if (code.empty())
		{
			error = "WEB_HDR_SHADER: missing HDR compositor WGSL";
			return false;
		}
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {code.data(), code.size()};
		WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shaderDescriptor.nextInChain = &source.chain;
		const auto shader = wgpuDeviceCreateShaderModule(device, &shaderDescriptor);
		if (!shader)
		{
			error = "WEB_HDR_SHADER: shader creation failed";
			return false;
		}
		for (unsigned pass = 0; pass < 2; ++pass)
		{
			std::array<WGPUBindGroupLayoutEntry, 2> entries{};
			entries[0].binding = 0;
			entries[0].visibility = WGPUShaderStage_Fragment;
			entries[0].buffer.type = WGPUBufferBindingType_Uniform;
			entries[0].buffer.minBindingSize = sizeof(gpu::HdrUniforms);
			entries[1].binding = 1;
			entries[1].visibility = WGPUShaderStage_Fragment;
			entries[1].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
			entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
			WGPUBindGroupLayoutDescriptor bindings = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
			bindings.entryCount = pass ? 2 : 1;
			bindings.entries = entries.data();
			auto& layout = pass ? m_composite_layout : m_background_layout;
			layout = wgpuDeviceCreateBindGroupLayout(device, &bindings);
			WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
			pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
			pipelineLayoutDescriptor.bindGroupLayouts = &layout;
			const auto pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
			WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
			color.format = pass ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_RGBA16Float;
			WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
			fragment.module = shader;
			fragment.entryPoint = pass ? WGPUStringView{"compositeMain", 13} : WGPUStringView{"backgroundMain", 14};
			fragment.targetCount = 1;
			fragment.targets = &color;
			WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
			descriptor.layout = pipelineLayout;
			descriptor.vertex.module = shader;
			descriptor.vertex.entryPoint = {"vertexMain", 10};
			descriptor.fragment = &fragment;
			descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
			auto& pipeline = pass ? m_composite : m_background;
			pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
			wgpuPipelineLayoutRelease(pipelineLayout);
		}
		wgpuShaderModuleRelease(shader);
		if (!m_background || !m_composite || !m_background_layout || !m_composite_layout)
		{
			error = "WEB_HDR_PIPELINE: compositor pipeline creation failed";
			Shutdown();
			return false;
		}
		return true;
	}

	std::expected<gpu::HdrPlan, std::string> WebHdrRenderer::Prepare(const RenderFramePacket& packet, unsigned width, unsigned height) const
	{
		if (!packet.darknessVolumes.empty())
		{
			return std::unexpected("WEB_HDR_EFFECT_UNSUPPORTED: darkness volumes require the separately qualified effects profile");
		}
		gpu::HdrUniforms constants{};
		const auto copy = [](const glm::vec4& value)
		{
			return std::array<float, 4>{value.x, value.y, value.z, value.w};
		};
		for (unsigned index = 0; index < 4; ++index)
		{
			constants.view[index] = copy(packet.view[index]);
		}
		constants.settings = {static_cast<float>(packet.tonemapMode), packet.tonemapExposure, static_cast<float>(packet.backgroundMode), static_cast<float>(packet.backgroundStopCount)};
		constants.extentAngleTime = {0, 0, packet.backgroundAngleRadians, packet.elapsedTime};
		constants.projection = {packet.proj[0][0], packet.proj[1][1], 0, 0};
		constants.sunDirectionIntensity = copy(packet.sunDirectionIntensity);
		constants.sunColor = copy(packet.sunColor);
		constants.horizon = copy(packet.skyHorizonColor);
		constants.zenith = copy(packet.skyZenithColor);
		constants.voidColor = copy(packet.skyVoidColor);
		constants.skyParams = copy(packet.skyParams);
		constants.shadingParams = copy(packet.shadingParams);
		constants.camera = copy(packet.cameraWorldPos);
		for (unsigned index = 0; index < constants.stops.size(); ++index)
		{
			constants.stops[index] = copy(packet.backgroundStops[index]);
		}
		return gpu::PlanHdrPass(constants, width, height);
	}

	std::expected<std::unique_ptr<WebHdrFrame>, std::string> WebHdrRenderer::Allocate(WGPUDevice device, WGPUQueue queue, const gpu::HdrPlan& plan) const
	{
		// Revalidate even callers constructing a plan directly; no allocation precedes this.
		const auto valid = gpu::PlanHdrPass(plan.constants, plan.width, plan.height);
		if (!valid)
		{
			return std::unexpected(valid.error());
		}
		if (!m_background || !m_composite)
		{
			return std::unexpected("WEB_HDR_PIPELINE: compositor is not initialized");
		}
		auto frame = std::make_unique<WebHdrFrame>();
		WGPUTextureDescriptor texture = WGPU_TEXTURE_DESCRIPTOR_INIT;
		texture.size = {plan.width, plan.height, 1};
		texture.dimension = WGPUTextureDimension_2D;
		texture.format = WGPUTextureFormat_RGBA16Float;
		texture.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
		frame->color = wgpuDeviceCreateTexture(device, &texture);
		if (!frame->color)
		{
			return std::unexpected("WEB_HDR_ALLOCATE: texture creation failed");
		}
		frame->view = wgpuTextureCreateView(frame->color, nullptr);
		WGPUBufferDescriptor buffer = WGPU_BUFFER_DESCRIPTOR_INIT;
		buffer.size = sizeof(gpu::HdrUniforms);
		buffer.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
		frame->uniform = wgpuDeviceCreateBuffer(device, &buffer);
		if (!frame->view || !frame->uniform)
		{
			return std::unexpected("WEB_HDR_ALLOCATE: uniform/view creation failed");
		}
		wgpuQueueWriteBuffer(queue, frame->uniform, 0, &valid->constants, sizeof(gpu::HdrUniforms));
		std::array<WGPUBindGroupEntry, 2> entries{};
		entries[0].binding = 0;
		entries[0].buffer = frame->uniform;
		entries[0].size = sizeof(gpu::HdrUniforms);
		entries[1].binding = 1;
		entries[1].textureView = frame->view;
		WGPUBindGroupDescriptor bindings = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindings.layout = m_background_layout;
		bindings.entryCount = 1;
		bindings.entries = entries.data();
		frame->backgroundBindings = wgpuDeviceCreateBindGroup(device, &bindings);
		bindings.layout = m_composite_layout;
		bindings.entryCount = 2;
		frame->compositeBindings = wgpuDeviceCreateBindGroup(device, &bindings);
		if (!frame->backgroundBindings || !frame->compositeBindings)
		{
			return std::unexpected("WEB_HDR_ALLOCATE: bind group creation failed");
		}
		return frame;
	}

	namespace
	{
		void Encode(WGPUCommandEncoder encoder, WGPUTextureView target, WGPURenderPipeline pipeline, WGPUBindGroup bindings)
		{
			WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			color.view = target;
			color.loadOp = WGPULoadOp_Clear;
			color.storeOp = WGPUStoreOp_Store;
			color.clearValue = {0, 0, 0, 0};
			WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			descriptor.colorAttachmentCount = 1;
			descriptor.colorAttachments = &color;
			const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
			wgpuRenderPassEncoderSetPipeline(pass, pipeline);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, bindings, 0, nullptr);
			wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
			wgpuRenderPassEncoderEnd(pass);
			wgpuRenderPassEncoderRelease(pass);
		}
	} // namespace

	void WebHdrRenderer::EncodeBackground(WGPUCommandEncoder encoder, const WebHdrFrame& frame) const
	{
		Encode(encoder, frame.view, m_background, frame.backgroundBindings);
	}

	void WebHdrRenderer::EncodeComposite(WGPUCommandEncoder encoder, WGPUTextureView canvas, const WebHdrFrame& frame) const
	{
		Encode(encoder, canvas, m_composite, frame.compositeBindings);
	}
} // namespace aether::webgpu
