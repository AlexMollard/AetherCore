#include "WebLight2DRenderer.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	namespace
	{
		struct LightRecord
		{
			glm::vec4 position, color, direction, flags;
		};

		struct LightUniform
		{
			std::array<glm::vec4, 4> view, inverse;
			glm::vec4 viewport, ambient, shadow, shaft;
		};

		static_assert(sizeof(LightRecord) == 64);
		static_assert(sizeof(LightUniform) == 192);

		bool Finite(glm::vec4 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		void Destroy(WGPUBuffer buffer)
		{
			if (buffer)
			{
				wgpuBufferDestroy(buffer);
				wgpuBufferRelease(buffer);
			}
		}

		WGPUBuffer Buffer(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* data, std::size_t size)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = size;
			descriptor.usage = usage | WGPUBufferUsage_CopyDst;
			const auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
			if (buffer)
			{
				wgpuQueueWriteBuffer(queue, buffer, 0, data, size);
			}
			return buffer;
		}

		WGPUShaderModule Shader(WGPUDevice device, const char* path)
		{
			std::ifstream input(path);
			const std::string code(std::istreambuf_iterator<char>(input), {});
			if (code.empty())
			{
				return nullptr;
			}
			WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
			source.code = {code.data(), code.size()};
			WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
			descriptor.nextInChain = &source.chain;
			return wgpuDeviceCreateShaderModule(device, &descriptor);
		}
	} // namespace

	WebLight2DFrame::~WebLight2DFrame()
	{
		for (const auto group: artwork)
		{
			wgpuBindGroupRelease(group);
		}
		if (lighting)
		{
			wgpuBindGroupRelease(lighting);
		}
		if (geometry)
		{
			wgpuBindGroupRelease(geometry);
		}
		Destroy(uniform);
		Destroy(lights);
		Destroy(occluders);
		if (maskView)
		{
			wgpuTextureViewRelease(maskView);
		}
		if (mask)
		{
			wgpuTextureDestroy(mask);
			wgpuTextureRelease(mask);
		}
	}

	WebLight2DRenderer::~WebLight2DRenderer()
	{
		Shutdown();
	}

	void WebLight2DRenderer::Shutdown()
	{
		if (m_lighting)
		{
			wgpuRenderPipelineRelease(m_lighting);
			m_lighting = nullptr;
		}
		if (m_occlusion)
		{
			wgpuRenderPipelineRelease(m_occlusion);
			m_occlusion = nullptr;
		}
		if (m_lighting_layout)
		{
			wgpuBindGroupLayoutRelease(m_lighting_layout);
			m_lighting_layout = nullptr;
		}
		if (m_geometry_layout)
		{
			wgpuBindGroupLayoutRelease(m_geometry_layout);
			m_geometry_layout = nullptr;
		}
		if (m_artwork_layout)
		{
			wgpuBindGroupLayoutRelease(m_artwork_layout);
			m_artwork_layout = nullptr;
		}
		if (m_sampler)
		{
			wgpuSamplerRelease(m_sampler);
			m_sampler = nullptr;
		}
	}

	bool WebLight2DRenderer::Initialize(WGPUDevice device)
	{
		const auto lightShader = Shader(device, "/assets/light2d.wgsl");
		const auto maskShader = Shader(device, "/assets/occluder2d.wgsl");
		if (!lightShader || !maskShader)
		{
			if (lightShader)
			{
				wgpuShaderModuleRelease(lightShader);
			}
			if (maskShader)
			{
				wgpuShaderModuleRelease(maskShader);
			}
			error = "Missing browser 2D lighting or occluder WGSL";
			return false;
		}
		std::array<WGPUBindGroupLayoutEntry, 4> entries{};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[0].buffer.type = WGPUBufferBindingType_Uniform;
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		entries[2].binding = 2;
		entries[2].visibility = WGPUShaderStage_Fragment;
		entries[2].texture.sampleType = WGPUTextureSampleType_Float;
		entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[3].binding = 3;
		entries[3].visibility = WGPUShaderStage_Fragment;
		entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
		WGPUBindGroupLayoutDescriptor layout = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layout.entryCount = 4;
		layout.entries = entries.data();
		m_lighting_layout = wgpuDeviceCreateBindGroupLayout(device, &layout);
		layout.entryCount = 2;
		m_geometry_layout = wgpuDeviceCreateBindGroupLayout(device, &layout);
		entries[2].binding = 0;
		entries[3].binding = 1;
		layout.entries = entries.data() + 2;
		m_artwork_layout = wgpuDeviceCreateBindGroupLayout(device, &layout);
		WGPUSamplerDescriptor sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
		sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
		sampler.addressModeU = sampler.addressModeV = WGPUAddressMode_ClampToEdge;
		m_sampler = wgpuDeviceCreateSampler(device, &sampler);
		for (unsigned pass = 0; pass < 2; ++pass)
		{
			std::array<WGPUBindGroupLayout, 2> layouts{m_geometry_layout, m_artwork_layout};
			if (pass == 1)
			{
				layouts[0] = m_lighting_layout;
			}
			WGPUPipelineLayoutDescriptor pipelineLayout = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
			pipelineLayout.bindGroupLayoutCount = pass == 0 ? 2 : 1;
			pipelineLayout.bindGroupLayouts = layouts.data();
			const auto pipelineLayoutHandle = wgpuDeviceCreatePipelineLayout(device, &pipelineLayout);
			WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
			blend.color.operation = pass == 0 ? WGPUBlendOperation_Max : WGPUBlendOperation_Add;
			blend.color.srcFactor = pass == 0 ? WGPUBlendFactor_One : WGPUBlendFactor_Dst;
			blend.color.dstFactor = pass == 0 ? WGPUBlendFactor_One : WGPUBlendFactor_Zero;
			blend.alpha.operation = WGPUBlendOperation_Add;
			blend.alpha.srcFactor = WGPUBlendFactor_Zero;
			blend.alpha.dstFactor = WGPUBlendFactor_One;
			WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
			target.format = pass == 0 ? WGPUTextureFormat_R8Unorm : WGPUTextureFormat_RGBA16Float;
			target.blend = pass == 0 ? nullptr : &blend;
			WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
			fragment.module = pass == 0 ? maskShader : lightShader;
			fragment.entryPoint = {"fragmentMain", 12};
			fragment.targetCount = 1;
			fragment.targets = &target;
			WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
			descriptor.layout = pipelineLayoutHandle;
			descriptor.vertex.module = fragment.module;
			descriptor.vertex.entryPoint = {"vertexMain", 10};
			descriptor.fragment = &fragment;
			descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
			descriptor.primitive.cullMode = WGPUCullMode_None;
			(pass == 0 ? m_occlusion : m_lighting) = wgpuDeviceCreateRenderPipeline(device, &descriptor);
			wgpuPipelineLayoutRelease(pipelineLayoutHandle);
		}
		wgpuShaderModuleRelease(lightShader);
		wgpuShaderModuleRelease(maskShader);
		return m_lighting && m_occlusion && m_sampler;
	}

	std::expected<bool, std::string> WebLight2DRenderer::Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures, unsigned width, unsigned height) const
	{
		const bool sceneSprites =
		        std::any_of(packet.render2D.sprites.begin(), packet.render2D.sprites.end(), [](const auto& sprite) { return (static_cast<std::uint32_t>(sprite.flags) & static_cast<std::uint32_t>(SpriteInstanceFlags::DisplaySpace)) == 0; });
		const bool active = sceneSprites && (packet.light2DHasSettings || !packet.pointLights.empty() || !packet.spotLights.empty());
		if (!active)
		{
			return false;
		}
		if (width == 0 || height == 0 || width > 8192 || height > 8192 || std::uint64_t(width) * height > 16777216)
		{
			return std::unexpected("Browser 2D lighting viewport exceeds the 16M-pixel budget");
		}
		if (packet.pointLights.size() + packet.spotLights.size() > 1024 || packet.render2D.occluders.size() > 65536)
		{
			return std::unexpected("Browser 2D lighting exceeds 1024 lights or 65536 occluders");
		}
		const auto projection = packet.proj * packet.view;
		if (std::abs(projection[0][3]) > 1e-6f || std::abs(projection[1][3]) > 1e-6f || std::abs(projection[2][3]) > 1e-6f || std::abs(projection[3][3] - 1.0f) > 1e-6f)
		{
			return std::unexpected("Browser 2D lighting requires an orthographic camera");
		}
		const auto inverse = glm::inverse(projection);
		for (unsigned i = 0; i < 4; ++i)
		{
			if (!Finite(projection[i]) || !Finite(inverse[i]))
			{
				return std::unexpected("Non-finite browser 2D lighting camera");
			}
		}
		if (!Finite(packet.light2DAmbient) || !Finite(packet.light2DShadowParams) || packet.light2DShadowParams.z != 0.0f)
		{
			return std::unexpected("Invalid or unsupported browser 2D lighting settings");
		}
		for (const auto& light: packet.pointLights)
		{
			if (!Finite(glm::vec4(light.position, light.radius)) || !Finite(glm::vec4(light.color, light.intensity)) || light.radius <= 0.0f)
			{
				return std::unexpected("Invalid browser 2D point light");
			}
		}
		for (const auto& light: packet.spotLights)
		{
			if (!Finite(glm::vec4(light.position, light.radius)) || !Finite(glm::vec4(light.color, light.intensity)) || !Finite(glm::vec4(light.direction, light.innerAngleRad)) || !std::isfinite(light.outerAngleRad) || light.radius <= 0.0f)
			{
				return std::unexpected("Invalid browser 2D spot light");
			}
		}
		for (const auto& occluder: packet.render2D.occluders)
		{
			const auto textureSlot = (occluder.flags & kOccluder2DCapsule) != 0 ? 1u : occluder.textureIndex;
			if (!Finite(occluder.posHalfSize) || !Finite(occluder.uvRect) || occluder.posHalfSize.z <= 0.0f || (occluder.flags & ~7u) != 0u
			        || textureSlot >= textures.size() || !textures[textureSlot])
			{
				return std::unexpected("Invalid browser 2D occluder or missing texture");
			}
		}
		return true;
	}

	void WebLight2DRenderer::Encode(
	        WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, WGPUTextureView hdrColor, unsigned width, unsigned height, const RenderFramePacket& packet, std::span<const WGPUTexture> textures, std::unique_ptr<WebLight2DFrame>& frame)
	{
		frame = std::make_unique<WebLight2DFrame>();
		auto& owned = *frame;
		std::vector<LightRecord> lights;
		for (const auto& light: packet.pointLights)
		{
			lights.push_back({glm::vec4(glm::vec2(light.position), light.radius, 0.0f), glm::vec4(light.color, light.intensity), {}, glm::vec4(light.castsShadow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f)});
		}
		for (const auto& light: packet.spotLights)
		{
			const auto xy = glm::vec2(light.direction);
			const float length = glm::length(xy);
			const auto direction = length > 1e-5f ? xy / length : glm::vec2(1.0f, 0.0f);
			lights.push_back({glm::vec4(glm::vec2(light.position), light.radius, 1.0f),
			        glm::vec4(light.color, light.intensity),
			        glm::vec4(direction, std::cos(light.innerAngleRad), std::cos(light.outerAngleRad)),
			        glm::vec4(light.castsShadow ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f)});
		}
		const auto lightCount = lights.size();
		if (lights.empty())
		{
			lights.emplace_back();
		}
		const auto projectionRows = glm::transpose(packet.proj * packet.view);
		const auto inverseRows = glm::transpose(glm::inverse(packet.proj * packet.view));
		LightUniform uniforms{};
		for (unsigned i = 0; i < 4; ++i)
		{
			uniforms.view[i] = projectionRows[i];
			uniforms.inverse[i] = inverseRows[i];
		}
		uniforms.viewport = glm::vec4(width, height, lightCount, packet.render2D.occluders.empty() ? 0.0f : 1.0f);
		uniforms.ambient = glm::vec4(glm::vec3(packet.light2DAmbient), 4.0f);
		uniforms.shadow = glm::vec4(1.0f, packet.light2DShadowParams.x, packet.light2DShadowParams.y, 0.0f);
		uniforms.shaft = glm::vec4(0.0f);
		owned.uniform = Buffer(device, queue, WGPUBufferUsage_Uniform, &uniforms, sizeof(uniforms));
		owned.lights = Buffer(device, queue, WGPUBufferUsage_Storage, lights.data(), lights.size() * sizeof(LightRecord));
		const Occluder2D dummy{};
		owned.occluders = Buffer(device, queue, WGPUBufferUsage_Storage, packet.render2D.occluders.empty() ? &dummy : packet.render2D.occluders.data(), std::max<std::size_t>(1, packet.render2D.occluders.size()) * sizeof(Occluder2D));
		WGPUTextureDescriptor mask = WGPU_TEXTURE_DESCRIPTOR_INIT;
		mask.size = {width, height, 1};
		mask.dimension = WGPUTextureDimension_2D;
		mask.format = WGPUTextureFormat_R8Unorm;
		mask.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment;
		owned.mask = wgpuDeviceCreateTexture(device, &mask);
		owned.maskView = wgpuTextureCreateView(owned.mask, nullptr);
		std::array<WGPUBindGroupEntry, 4> entries{};
		entries[0].binding = 0;
		entries[0].buffer = owned.uniform;
		entries[0].size = sizeof(uniforms);
		entries[1].binding = 1;
		entries[1].buffer = owned.occluders;
		entries[1].size = std::max<std::size_t>(1, packet.render2D.occluders.size()) * sizeof(Occluder2D);
		WGPUBindGroupDescriptor group = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		group.layout = m_geometry_layout;
		group.entryCount = 2;
		group.entries = entries.data();
		owned.geometry = wgpuDeviceCreateBindGroup(device, &group);
		entries[1].buffer = owned.lights;
		entries[1].size = lights.size() * sizeof(LightRecord);
		entries[2].binding = 2;
		entries[2].textureView = owned.maskView;
		entries[3].binding = 3;
		entries[3].sampler = m_sampler;
		group.layout = m_lighting_layout;
		group.entryCount = 4;
		owned.lighting = wgpuDeviceCreateBindGroup(device, &group);
		WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		attachment.view = owned.maskView;
		attachment.loadOp = WGPULoadOp_Clear;
		attachment.storeOp = WGPUStoreOp_Store;
		attachment.clearValue = {0.0, 0.0, 0.0, 0.0};
		WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		pass.colorAttachmentCount = 1;
		pass.colorAttachments = &attachment;
		const auto maskPass = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
		wgpuRenderPassEncoderSetPipeline(maskPass, m_occlusion);
		wgpuRenderPassEncoderSetBindGroup(maskPass, 0, owned.geometry, 0, nullptr);
		for (unsigned i = 0; i < packet.render2D.occluders.size(); ++i)
		{
			const auto& occluder = packet.render2D.occluders[i];
			const auto slot = (occluder.flags & kOccluder2DCapsule) != 0 ? 1u : occluder.textureIndex;
			const auto textureView = wgpuTextureCreateView(textures[slot], nullptr);
			std::array<WGPUBindGroupEntry, 2> artwork{};
			artwork[0].binding = 0;
			artwork[0].textureView = textureView;
			artwork[1].binding = 1;
			artwork[1].sampler = m_sampler;
			group.layout = m_artwork_layout;
			group.entryCount = 2;
			group.entries = artwork.data();
			const auto material = wgpuDeviceCreateBindGroup(device, &group);
			owned.artwork.push_back(material);
			wgpuTextureViewRelease(textureView);
			wgpuRenderPassEncoderSetBindGroup(maskPass, 1, material, 0, nullptr);
			wgpuRenderPassEncoderDraw(maskPass, 6, 1, 0, i);
		}
		wgpuRenderPassEncoderEnd(maskPass);
		wgpuRenderPassEncoderRelease(maskPass);
		attachment.view = hdrColor;
		attachment.loadOp = WGPULoadOp_Load;
		const auto lightPass = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
		wgpuRenderPassEncoderSetPipeline(lightPass, m_lighting);
		wgpuRenderPassEncoderSetBindGroup(lightPass, 0, owned.lighting, 0, nullptr);
		wgpuRenderPassEncoderDraw(lightPass, 3, 1, 0, 0);
		wgpuRenderPassEncoderEnd(lightPass);
		wgpuRenderPassEncoderRelease(lightPass);
	}
} // namespace aether::webgpu
