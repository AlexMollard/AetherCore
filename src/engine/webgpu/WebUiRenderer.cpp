#include "WebUiRenderer.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <glm/glm.hpp>

namespace aether::webgpu
{
	namespace
	{
		struct Parameters { glm::vec4 rect{0.f}, params{0.f}, color0{0.f}, color1{0.f}; };
		static_assert(sizeof(Parameters) == 64);
		WGPUBuffer Buffer(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* data, std::size_t bytes)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = bytes;
			descriptor.usage = usage | WGPUBufferUsage_CopyDst;
			auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
			if (buffer) wgpuQueueWriteBuffer(queue, buffer, 0, data, bytes);
			return buffer;
		}
		WGPUTexture CurveTexture(WGPUDevice device, WGPUQueue queue, unsigned width, unsigned height, const void* data)
		{
			WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
			desc.dimension = WGPUTextureDimension_2D;
			desc.size = {width, height, 1};
			desc.format = WGPUTextureFormat_RGBA32Float;
			desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
			auto texture = wgpuDeviceCreateTexture(device, &desc);
			if (!texture) return nullptr;
			WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			destination.texture = texture;
			WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
			layout.bytesPerRow = width * sizeof(glm::vec4);
			layout.rowsPerImage = height;
			const WGPUExtent3D extent{width, height, 1};
			wgpuQueueWriteTexture(queue, &destination, data, std::size_t(width) * height * sizeof(glm::vec4), &layout, &extent);
			return texture;
		}
	}

	WebUiFrame::~WebUiFrame()
	{
		for (auto group: bindings) wgpuBindGroupRelease(group);
		for (auto buffer: buffers) { wgpuBufferDestroy(buffer); wgpuBufferRelease(buffer); }
	}
	WebUiRenderer::~WebUiRenderer() { Shutdown(); }
	void WebUiRenderer::Shutdown()
	{
		for (auto& [name, pipeline]: m_materials) wgpuRenderPipelineRelease(pipeline);
		for (auto& [name, pipeline]: m_effects) wgpuRenderPipelineRelease(pipeline);
		m_materials.clear(); m_effects.clear();
		if (m_default) wgpuRenderPipelineRelease(m_default);
		if (m_builtinShader) wgpuShaderModuleRelease(m_builtinShader);
		if (m_pipelineLayout) wgpuPipelineLayoutRelease(m_pipelineLayout);
		if (m_layout) wgpuBindGroupLayoutRelease(m_layout);
		for (auto texture: m_fonts) if (texture) { wgpuTextureDestroy(texture); wgpuTextureRelease(texture); }
		m_fonts.clear();
		if (m_dummyCurve) { wgpuTextureDestroy(m_dummyCurve); wgpuTextureRelease(m_dummyCurve); }
		m_default = nullptr; m_builtinShader = nullptr; m_pipelineLayout = nullptr; m_layout = nullptr; m_dummyCurve = nullptr;
	}
	bool WebUiRenderer::Initialize(WGPUDevice device, WGPUQueue queue)
	{
		std::array<WGPUBindGroupLayoutEntry, 6> entries{};
		for (unsigned i = 0; i < entries.size(); ++i) { entries[i].binding = i; entries[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment; }
		entries[0].buffer.type = WGPUBufferBindingType_Uniform; entries[0].buffer.minBindingSize = 16;
		entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage; entries[1].buffer.minBindingSize = sizeof(ui::UiDrawCommand);
		entries[2].texture.sampleType = WGPUTextureSampleType_Float; entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[4].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat; entries[4].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[5].buffer.type = WGPUBufferBindingType_Uniform; entries[5].buffer.minBindingSize = sizeof(Parameters);
		WGPUBindGroupLayoutDescriptor group = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		group.entryCount = entries.size(); group.entries = entries.data();
		m_layout = wgpuDeviceCreateBindGroupLayout(device, &group);
		WGPUPipelineLayoutDescriptor layout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		layout.bindGroupLayoutCount = 1; layout.bindGroupLayouts = &m_layout;
		m_pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &layout);
		std::ifstream input("/assets/ui_shapes_web.wgsl");
		const std::string code(std::istreambuf_iterator<char>(input), {});
		if (code.empty()) { error = "Missing UI shader"; return false; }
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {code.data(), code.size()};
		WGPUShaderModuleDescriptor shader = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shader.nextInChain = &source.chain;
		m_builtinShader = wgpuDeviceCreateShaderModule(device, &shader);
		m_default = CreatePipeline(device, code, false);
		const glm::vec4 zero(0.f);
		m_dummyCurve = CurveTexture(device, queue, 1, 1, &zero);
		if (!m_layout || !m_pipelineLayout || !m_builtinShader || !m_default || !m_dummyCurve) { error = "UI renderer initialization failed"; return false; }
		return true;
	}
	WGPURenderPipeline WebUiRenderer::CreatePipeline(WGPUDevice device, std::string_view code, bool effect)
	{
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {code.data(), code.size()};
		WGPUShaderModuleDescriptor shader = WGPU_SHADER_MODULE_DESCRIPTOR_INIT; shader.nextInChain = &source.chain;
		auto module = wgpuDeviceCreateShaderModule(device, &shader);
		if (!module) return nullptr;
		WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
		blend.color.operation = blend.alpha.operation = WGPUBlendOperation_Add;
		blend.color.srcFactor = WGPUBlendFactor_SrcAlpha; blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.alpha.srcFactor = WGPUBlendFactor_One; blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT; target.format = WGPUTextureFormat_RGBA8Unorm; target.blend = &blend;
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT; fragment.module = module; fragment.entryPoint = {"fragmentMain", 12}; fragment.targetCount = 1; fragment.targets = &target;
		WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
		desc.layout = m_pipelineLayout; desc.vertex.module = effect ? module : m_builtinShader; desc.vertex.entryPoint = {"vertexMain", 10};
		desc.fragment = &fragment; desc.primitive.topology = WGPUPrimitiveTopology_TriangleList; desc.primitive.cullMode = WGPUCullMode_None;
		auto pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
		wgpuShaderModuleRelease(module);
		return pipeline;
	}
	bool WebUiRenderer::UploadFont(WGPUDevice device, WGPUQueue queue, ui::FontAsset& font)
	{
		if (font.curveBindlessSlot != 0xffffffffu) return FontTexture(font.curveBindlessSlot) != nullptr;
		if (!font.textureWidth || !font.textureHeight || font.texels.size() != std::uint64_t(font.textureWidth) * font.textureHeight || m_fonts.size() >= 0xffffffffu) { error = "Invalid UI font curve payload"; return false; }
		auto texture = CurveTexture(device, queue, font.textureWidth, font.textureHeight, font.texels.data());
		if (!texture) { error = "UI font curve upload failed"; return false; }
		font.curveBindlessSlot = static_cast<std::uint32_t>(m_fonts.size());
		m_fonts.push_back(texture);
		return true;
	}
	bool WebUiRenderer::AllocateFont(WGPUDevice device, ui::FontAsset& font)
	{
		if (font.curveBindlessSlot != 0xffffffffu) return FontTexture(font.curveBindlessSlot) != nullptr;
		if (!font.textureWidth || !font.textureHeight || font.texels.size() != std::uint64_t(font.textureWidth) * font.textureHeight || m_fonts.size() >= 0xffffffffu)
		{
			error = "Invalid UI font curve payload"; return false;
		}
		WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
		desc.dimension = WGPUTextureDimension_2D;
		desc.size = {font.textureWidth, font.textureHeight, 1};
		desc.format = WGPUTextureFormat_RGBA32Float;
		desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		const auto texture = wgpuDeviceCreateTexture(device, &desc);
		if (!texture) { error = "UI font curve allocation failed"; return false; }
		const auto vacant = std::find(m_fonts.begin(), m_fonts.end(), nullptr);
		if (vacant == m_fonts.end()) { font.curveBindlessSlot = static_cast<std::uint32_t>(m_fonts.size()); m_fonts.push_back(texture); }
		else { font.curveBindlessSlot = static_cast<std::uint32_t>(vacant - m_fonts.begin()); *vacant = texture; }
		return true;
	}
	WGPUTexture WebUiRenderer::FontTexture(std::uint32_t slot) const
	{
		return slot < m_fonts.size() ? m_fonts[slot] : nullptr;
	}
	void WebUiRenderer::ReleaseFont(std::uint32_t slot)
	{
		if (auto texture = FontTexture(slot)) { wgpuTextureDestroy(texture); wgpuTextureRelease(texture); m_fonts[slot] = nullptr; }
	}
	bool WebUiRenderer::HasShader(std::string_view stem, bool effect, bool glyph) const
	{
		const auto& shaders = effect ? m_effects : m_materials;
		return shaders.find(effect ? std::string(stem) : std::string(1, glyph ? 'g' : 'i') + std::string(stem)) != shaders.end();
	}
	bool WebUiRenderer::UploadShader(WGPUDevice device, std::string_view stem, bool effect, bool glyph, std::string_view source)
	{
		auto& shaders = effect ? m_effects : m_materials;
		if (HasShader(stem, effect, glyph)) return true;
		if (stem.empty() || source.empty()) { error = "Missing authored UI shader source"; return false; }
		auto pipeline = CreatePipeline(device, source, effect);
		if (!pipeline) { error = "Unable to create authored UI pipeline: " + std::string(stem); return false; }
		shaders.emplace(effect ? std::string(stem) : std::string(1, glyph ? 'g' : 'i') + std::string(stem), pipeline);
		return true;
	}
	bool WebUiRenderer::Encode(WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, WGPUTextureView target,
	        unsigned width, unsigned height, const ui::UiFrameData& data, const std::vector<WGPUTexture>& images,
	        WGPUSampler sampler, std::unique_ptr<WebUiFrame>& frame)
	{
		if (data.commands.empty() && data.effects.empty()) return true;
		if (images.size() <= 1 || !images[1]) { error = "UI requires the default white texture"; return false; }
		frame = std::make_unique<WebUiFrame>();
		const glm::vec4 screen(width, height, 0.f, 0.f);
		const ui::UiDrawCommand dummy{};
		auto uniform = Buffer(device, queue, WGPUBufferUsage_Uniform, &screen, sizeof(screen));
		auto commands = Buffer(device, queue, WGPUBufferUsage_Storage, data.commands.empty() ? &dummy : data.commands.data(),
		        std::max<std::size_t>(1, data.commands.size()) * sizeof(dummy));
		if (uniform) frame->buffers.push_back(uniform);
		if (commands) frame->buffers.push_back(commands);
		if (!uniform || !commands) { error = "UI buffer allocation failed"; return false; }
		WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		attachment.view = target; attachment.loadOp = WGPULoadOp_Load; attachment.storeOp = WGPUStoreOp_Store;
		WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		passDesc.colorAttachmentCount = 1; passDesc.colorAttachments = &attachment;
		auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
		const auto draw = [&](WGPURenderPipeline pipeline, WGPUTexture image, WGPUTexture curve, const Parameters& params, unsigned first, unsigned count)
		{
			auto paramBuffer = Buffer(device, queue, WGPUBufferUsage_Uniform, &params, sizeof(params));
			if (!paramBuffer) return false;
			frame->buffers.push_back(paramBuffer);
			auto imageView = wgpuTextureCreateView(image, nullptr);
			auto curveView = wgpuTextureCreateView(curve, nullptr);
			std::array<WGPUBindGroupEntry, 6> entries{};
			for (unsigned i = 0; i < entries.size(); ++i) entries[i].binding = i;
			entries[0].buffer = uniform; entries[0].size = sizeof(screen);
			entries[1].buffer = commands; entries[1].size = std::max<std::size_t>(1, data.commands.size()) * sizeof(dummy);
			entries[2].textureView = imageView; entries[3].sampler = sampler; entries[4].textureView = curveView;
			entries[5].buffer = paramBuffer; entries[5].size = sizeof(params);
			WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			desc.layout = m_layout; desc.entryCount = entries.size(); desc.entries = entries.data();
			auto group = wgpuDeviceCreateBindGroup(device, &desc);
			wgpuTextureViewRelease(imageView); wgpuTextureViewRelease(curveView);
			if (!group) return false;
			frame->bindings.push_back(group);
			wgpuRenderPassEncoderSetPipeline(pass, pipeline);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
			wgpuRenderPassEncoderDraw(pass, 6, count, 0, first);
			return true;
		};
		const auto effects = [&](bool background)
		{
			for (const auto& effect: data.effects)
			{
				if (effect.background != background) continue;
				const auto it = m_effects.find(effect.shader);
				if (it == m_effects.end()) { error = "Missing UI effect source: " + effect.shader; return false; }
				if (!draw(it->second, images[1], m_dummyCurve, {effect.rect, effect.params, effect.color0, effect.color1}, 0, 1)) return false;
			}
			return true;
		};
		bool ok = effects(true);
		for (std::size_t i = 0; ok && i < data.commands.size();)
		{
			const auto& command = data.commands[i];
			const auto shaderId = ui::UiFlagsShaderId(command.flags);
			auto pipeline = m_default;
			Parameters params{};
			if (shaderId)
			{
				if (shaderId > data.materials.size()) { error = "Invalid UI material index"; ok = false; break; }
				const auto& material = data.materials[shaderId - 1];
				const auto it = m_materials.find(std::string(1, command.type == ui::kShapeGlyph ? 'g' : 'i') + material.shader);
				if (it == m_materials.end()) { error = "Missing UI material source: " + material.shader; ok = false; break; }
				pipeline = it->second;
				params.params = material.params; params.color0 = material.color0; params.color1 = material.color1;
			}
			WGPUTexture image = images[1], curve = m_dummyCurve;
			if (command.type == ui::kShapeGlyph)
			{
				if (command.textureSlot >= m_fonts.size()) { error = "Invalid UI font slot"; ok = false; break; }
				curve = m_fonts[command.textureSlot];
			}
			else if (command.type == ui::kShapeTexturedRect)
			{
				if (command.textureSlot >= images.size() || !images[command.textureSlot]) { error = "Invalid UI image slot"; ok = false; break; }
				image = images[command.textureSlot];
			}
			std::size_t end = i + 1;
			while (end < data.commands.size() && data.commands[end].type == command.type &&
			        data.commands[end].textureSlot == command.textureSlot && ui::UiFlagsShaderId(data.commands[end].flags) == shaderId) ++end;
			ok = draw(pipeline, image, curve, params, static_cast<unsigned>(i), static_cast<unsigned>(end - i));
			i = end;
		}
		if (ok) ok = effects(false);
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
		if (!ok && error.empty()) error = "UI bind group creation failed";
		return ok;
	}
}
