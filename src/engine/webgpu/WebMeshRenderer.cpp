#include "WebMeshRenderer.hpp"

#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	namespace
	{
		WGPUBuffer Buffer(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* data, std::size_t bytes)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = bytes;
			descriptor.usage = usage | WGPUBufferUsage_CopyDst;
			const auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
			if (buffer)
			{
				wgpuQueueWriteBuffer(queue, buffer, 0, data, bytes);
			}
			return buffer;
		}

		void Destroy(WGPUBuffer buffer)
		{
			if (buffer)
			{
				wgpuBufferDestroy(buffer);
				wgpuBufferRelease(buffer);
			}
		}
	} // namespace

	WebMeshFrame::~WebMeshFrame()
	{
		if (bindings)
		{
			wgpuBindGroupRelease(bindings);
		}
		Destroy(uniform);
		Destroy(instances);
		Destroy(skin);
		for (auto material: materials)
		{
			wgpuBindGroupRelease(material);
		}
		if (depthView)
		{
			wgpuTextureViewRelease(depthView);
		}
		if (depth)
		{
			wgpuTextureDestroy(depth);
			wgpuTextureRelease(depth);
		}
	}

	WebMeshRenderer::~WebMeshRenderer()
	{
		Shutdown();
	}

	void WebMeshRenderer::Shutdown()
	{
		for (const auto& mesh: m_meshes)
		{
			Destroy(mesh.vertices);
			Destroy(mesh.indices);
		}
		m_meshes.clear();
		m_bytes = 0;
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
		if (m_material_layout)
		{
			wgpuBindGroupLayoutRelease(m_material_layout);
			m_material_layout = nullptr;
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

	bool WebMeshRenderer::Initialize(WGPUDevice device)
	{
		std::ifstream input("/assets/mesh.wgsl");
		const std::string code(std::istreambuf_iterator<char>(input), {});
		if (code.empty())
		{
			error = "Missing mesh WGSL";
			return false;
		}
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {code.data(), code.size()};
		WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shaderDescriptor.nextInChain = &source.chain;
		const auto shader = wgpuDeviceCreateShaderModule(device, &shaderDescriptor);
		std::array<WGPUVertexAttribute, 5> attributes{};
		attributes[0].format = WGPUVertexFormat_Float32x3;
		attributes[0].offset = offsetof(MeshVertex, position);
		attributes[0].shaderLocation = 0;
		attributes[1].format = WGPUVertexFormat_Float32x3;
		attributes[1].offset = offsetof(MeshVertex, color);
		attributes[1].shaderLocation = 1;
		attributes[2].format = WGPUVertexFormat_Float32x2;
		attributes[2].offset = offsetof(MeshVertex, uv);
		attributes[2].shaderLocation = 2;
		attributes[3].format = WGPUVertexFormat_Uint32x4;
		attributes[3].offset = offsetof(MeshVertex, jointIndices);
		attributes[3].shaderLocation = 3;
		attributes[4].format = WGPUVertexFormat_Float32x4;
		attributes[4].offset = offsetof(MeshVertex, jointWeights);
		attributes[4].shaderLocation = 4;
		WGPUVertexBufferLayout vertices = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
		vertices.arrayStride = sizeof(MeshVertex);
		vertices.stepMode = WGPUVertexStepMode_Vertex;
		vertices.attributeCount = attributes.size();
		vertices.attributes = attributes.data();
		WGPUColorTargetState color = WGPU_COLOR_TARGET_STATE_INIT;
		color.format = WGPUTextureFormat_RGBA8Unorm;
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
		fragment.module = shader;
		fragment.entryPoint = {"fragmentMain", 12};
		fragment.targetCount = 1;
		fragment.targets = &color;
		WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
		depth.format = WGPUTextureFormat_Depth32Float;
		depth.depthWriteEnabled = WGPUOptionalBool_True;
		depth.depthCompare = WGPUCompareFunction_Less;
		WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
		descriptor.vertex.module = shader;
		descriptor.vertex.entryPoint = {"vertexMain", 10};
		descriptor.vertex.bufferCount = 1;
		descriptor.vertex.buffers = &vertices;
		descriptor.fragment = &fragment;
		descriptor.depthStencil = &depth;
		descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		descriptor.primitive.cullMode = WGPUCullMode_None;
		// A shared explicit layout makes every blend/winding variant bind-compatible.
		std::array<WGPUBindGroupLayoutEntry, 3> frameEntries{};
		for (unsigned i = 0; i < frameEntries.size(); ++i)
		{
			frameEntries[i].binding = i;
			frameEntries[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
			frameEntries[i].buffer.type = i == 0 ? WGPUBufferBindingType_Uniform : WGPUBufferBindingType_ReadOnlyStorage;
		}
		WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.entryCount = frameEntries.size();
		layoutDescriptor.entries = frameEntries.data();
		m_frame_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		std::array<WGPUBindGroupLayoutEntry, 3> materialEntries{};
		for (unsigned i = 0; i < 2; ++i)
		{
			materialEntries[i].binding = i;
			materialEntries[i].visibility = WGPUShaderStage_Fragment;
			materialEntries[i].texture.sampleType = WGPUTextureSampleType_Float;
			materialEntries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		materialEntries[2].binding = 2;
		materialEntries[2].visibility = WGPUShaderStage_Fragment;
		materialEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;
		layoutDescriptor.entries = materialEntries.data();
		m_material_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		std::array<WGPUBindGroupLayout, 2> layouts{m_frame_layout, m_material_layout};
		WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pipelineLayoutDescriptor.bindGroupLayoutCount = layouts.size();
		pipelineLayoutDescriptor.bindGroupLayouts = layouts.data();
		const auto pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
		descriptor.layout = pipelineLayout;
		for (unsigned blend = 0; blend < 3; ++blend)
		{
			WGPUBlendState blending = WGPU_BLEND_STATE_INIT;
			blending.color.operation = blending.alpha.operation = WGPUBlendOperation_Add;
			blending.color.srcFactor = WGPUBlendFactor_SrcAlpha;
			blending.color.dstFactor = blend == 2 ? WGPUBlendFactor_One : WGPUBlendFactor_OneMinusSrcAlpha;
			blending.alpha.srcFactor = WGPUBlendFactor_One;
			blending.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
			color.blend = blend ? &blending : nullptr;
			depth.depthWriteEnabled = blend ? WGPUOptionalBool_False : WGPUOptionalBool_True;
			for (unsigned winding = 0; winding < 3; ++winding)
			{
				descriptor.primitive.cullMode = winding == 0 ? WGPUCullMode_None : WGPUCullMode_Back;
				descriptor.primitive.frontFace = winding == 2 ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
				m_pipelines[blend * 3 + winding] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
			}
		}
		wgpuPipelineLayoutRelease(pipelineLayout);
		WGPUSamplerDescriptor sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
		// Published materials use repeat addressing and trilinear mip filtering.
		sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_Repeat;
		sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
		sampler.mipmapFilter = WGPUMipmapFilterMode_Linear;
		sampler.lodMinClamp = 0;
		sampler.lodMaxClamp = 32;
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
		WGPUTexelCopyBufferLayout pixelLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		pixelLayout.bytesPerRow = 4;
		pixelLayout.rowsPerImage = 1;
		const auto queue = wgpuDeviceGetQueue(device);
		wgpuQueueWriteTexture(queue, &destination, pixel.data(), pixel.size(), &pixelLayout, &white.size);
		wgpuQueueRelease(queue);
		wgpuShaderModuleRelease(shader);
		return m_white && m_sampler && std::all_of(m_pipelines.begin(), m_pipelines.end(), [](auto pipeline) { return pipeline != nullptr; });
	}

	bool WebMeshRenderer::Upload(WGPUDevice device, WGPUQueue queue, std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices)
	{
		error.clear();
		constexpr std::size_t kBudget = 64 * 1024 * 1024;
		if (slot >= 1024 || vertices.empty() || indices.empty() || indices.size() % 3 || vertices.size() > 262144 || indices.size() > 786432)
		{
			error = "Invalid mesh slot, topology or upload limits";
			return false;
		}
		const auto bytes = vertices.size_bytes() + indices.size_bytes();
		if (bytes > kBudget - m_bytes || (slot < m_meshes.size() && m_meshes[slot].vertices))
		{
			error = "Mesh upload budget exceeded or immutable slot already populated";
			return false;
		}
		for (const auto index: indices)
		{
			if (index >= vertices.size())
			{
				error = "Mesh index outside uploaded vertices";
				return false;
			}
		}
		std::uint32_t jointCount = 0;
		bool skinReady = true;
		for (const auto& vertex: vertices)
		{
			if (!gpu::ValidMeshSkinInfluences(vertex.jointIndices, vertex.jointWeights, 256, false))
			{
				error = "Invalid mesh skin influences";
				return false;
			}
			skinReady = skinReady && gpu::ValidMeshSkinInfluences(vertex.jointIndices, vertex.jointWeights, 256);
			for (int component = 0; component < 4; ++component)
			{
				if (vertex.jointWeights[component] > 0 && vertex.jointIndices[component] < 256)
				{
					jointCount = std::max(jointCount, vertex.jointIndices[component] + 1);
				}
			}
			// Normal, tangent and UV2 are preserved but not read by this unlit shader.
			if (!std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y))
			{
				error = "Nonfinite mesh UV";
				return false;
			}
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.color[axis]))
				{
					error = "Nonfinite mesh vertex";
					return false;
				}
			}
		}
		if (slot >= m_meshes.size())
		{
			m_meshes.resize(slot + 1);
		}
		auto& mesh = m_meshes[slot];
		mesh.vertices = Buffer(device, queue, WGPUBufferUsage_Vertex, vertices.data(), vertices.size_bytes());
		mesh.indices = Buffer(device, queue, WGPUBufferUsage_Index, indices.data(), indices.size_bytes());
		mesh.indexCount = static_cast<std::uint32_t>(indices.size());
		mesh.jointCount = jointCount;
		mesh.skinReady = skinReady;
		if (!mesh.vertices || !mesh.indices)
		{
			Destroy(mesh.vertices);
			Destroy(mesh.indices);
			mesh = {};
			error = "Mesh buffer allocation failed";
			return false;
		}
		m_bytes += bytes;
		return true;
	}

	std::expected<std::vector<gpu::MeshGpuInstance>, std::string> WebMeshRenderer::Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures) const
	{
		const auto instances = gpu::PlanMeshPass(packet.render3D);
		if (!instances)
		{
			return std::unexpected(instances.error());
		}
		for (const auto& instance: packet.render3D.meshes)
		{
			if (instance.mesh >= m_meshes.size() || !m_meshes[instance.mesh].vertices)
			{
				return std::unexpected("Draw references an unregistered mesh");
			}
			if (!instance.skinMatrices.empty() && (!m_meshes[instance.mesh].skinReady || m_meshes[instance.mesh].jointCount > instance.skinMatrices.size()))
			{
				return std::unexpected("Mesh has unskinned vertices or joint index outside owned skin palette");
			}
			for (std::size_t i = 0; i < instance.textures.size(); ++i)
			{
				const auto texture = instance.textures[i];
				if (texture != UINT32_MAX && (texture >= textures.size() || !textures[texture]))
				{
					return std::unexpected("Mesh material references an unregistered texture");
				}
				if (texture != UINT32_MAX && wgpuTextureGetFormat(textures[texture]) != ((i == 0 || i == 4) ? WGPUTextureFormat_RGBA8UnormSrgb : WGPUTextureFormat_RGBA8Unorm))
				{
					return std::unexpected("Mesh material texture color space mismatch");
				}
			}
			const auto origin = packet.view * instance.world[3];
			for (int c = 0; c < 4; ++c)
			{
				if (!std::isfinite(origin[c]))
				{
					return std::unexpected("Nonfinite mesh camera depth");
				}
			}
			if (!std::isfinite(instance.material.uvScroll.x * packet.elapsedTime) || !std::isfinite(instance.material.uvScroll.y * packet.elapsedTime))
			{
				return std::unexpected("Nonfinite mesh UV offset");
			}
		}
		if (!std::isfinite(packet.elapsedTime))
		{
			return std::unexpected("Nonfinite mesh elapsed time");
		}
		const auto clip = glm::transpose(packet.proj * packet.view);
		for (int column = 0; column < 4; ++column)
		{
			for (int row = 0; row < 4; ++row)
			{
				if (!std::isfinite(clip[column][row]))
				{
					return std::unexpected("Nonfinite mesh camera matrix");
				}
			}
		}
		return instances;
	}

	void WebMeshRenderer::Encode(WGPUDevice device,
	        WGPUQueue queue,
	        WGPUCommandEncoder encoder,
	        WGPUTextureView color,
	        unsigned width,
	        unsigned height,
	        const RenderFramePacket& packet,
	        std::span<const gpu::MeshGpuInstance> instances,
	        std::span<const WGPUTexture> textures,
	        std::unique_ptr<WebMeshFrame>& frame)
	{
		const auto clip = glm::transpose(packet.proj * packet.view);
		frame = std::make_unique<WebMeshFrame>();
		const std::array<glm::vec4, 5> constants{clip[0], clip[1], clip[2], clip[3], glm::vec4(packet.elapsedTime, 0, 0, 0)};
		frame->uniform = Buffer(device, queue, WGPUBufferUsage_Uniform, constants.data(), sizeof(constants));
		frame->instances = Buffer(device, queue, WGPUBufferUsage_Storage, instances.data(), instances.size_bytes());
		std::vector<glm::mat4> palette;
		for (const auto& mesh: packet.render3D.meshes)
		{
			palette.insert(palette.end(), mesh.skinMatrices.begin(), mesh.skinMatrices.end());
		}
		if (palette.empty())
		{
			palette.emplace_back(1.0f);
		}
		frame->skin = Buffer(device, queue, WGPUBufferUsage_Storage, palette.data(), palette.size() * sizeof(glm::mat4));
		WGPUTextureDescriptor depth = WGPU_TEXTURE_DESCRIPTOR_INIT;
		depth.size = {width, height, 1};
		depth.dimension = WGPUTextureDimension_2D;
		depth.format = WGPUTextureFormat_Depth32Float;
		depth.usage = WGPUTextureUsage_RenderAttachment;
		frame->depth = wgpuDeviceCreateTexture(device, &depth);
		frame->depthView = wgpuTextureCreateView(frame->depth, nullptr);
		std::array<WGPUBindGroupEntry, 3> entries{};
		entries[0].binding = 0;
		entries[0].buffer = frame->uniform;
		entries[0].size = sizeof(constants);
		entries[1].binding = 1;
		entries[1].buffer = frame->instances;
		entries[1].size = instances.size_bytes();
		entries[2].binding = 2;
		entries[2].buffer = frame->skin;
		entries[2].size = palette.size() * sizeof(glm::mat4);
		WGPUBindGroupDescriptor bindings = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindings.layout = m_frame_layout;
		bindings.entries = entries.data();
		bindings.entryCount = entries.size();
		frame->bindings = wgpuDeviceCreateBindGroup(device, &bindings);
		WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		depthAttachment.view = frame->depthView;
		depthAttachment.depthLoadOp = WGPULoadOp_Clear;
		depthAttachment.depthStoreOp = WGPUStoreOp_Store;
		depthAttachment.depthClearValue = 1;
		WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		colorAttachment.view = color;
		colorAttachment.loadOp = WGPULoadOp_Clear;
		colorAttachment.storeOp = WGPUStoreOp_Store;
		colorAttachment.clearValue = {0, 0, 0, 1};
		WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		descriptor.colorAttachmentCount = 1;
		descriptor.colorAttachments = &colorAttachment;
		descriptor.depthStencilAttachment = &depthAttachment;
		const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
		for (const auto index: gpu::MeshDrawOrder(packet.render3D, packet.view))
		{
			const auto& instance = packet.render3D.meshes[index];
			const auto& mesh = m_meshes[instance.mesh];
			const auto flags = instances[index].metadata.w;
			const unsigned blend = instance.material.alphaBlend ? (instance.material.additiveBlend ? 2 : 1) : 0;
			const unsigned winding = instance.material.doubleSided ? 0 : ((flags & gpu::kMeshMirrored) ? 2 : 1);
			wgpuRenderPassEncoderSetPipeline(pass, m_pipelines[blend * 3 + winding]);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, frame->bindings, 0, nullptr);
			std::array<WGPUBindGroupEntry, 3> materialEntries{};
			std::array<WGPUTextureView, 2> views{};
			for (unsigned i = 0; i < 2; ++i)
			{
				const auto slot = instance.textures[i == 0 ? 0 : 4];
				views[i] = wgpuTextureCreateView(slot == UINT32_MAX ? m_white : textures[slot], nullptr);
				materialEntries[i].binding = i;
				materialEntries[i].textureView = views[i];
			}
			materialEntries[2].binding = 2;
			materialEntries[2].sampler = m_sampler;
			WGPUBindGroupDescriptor material = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			material.layout = m_material_layout;
			material.entries = materialEntries.data();
			material.entryCount = materialEntries.size();
			const auto group = wgpuDeviceCreateBindGroup(device, &material);
			frame->materials.push_back(group);
			for (auto view: views)
			{
				wgpuTextureViewRelease(view);
			}
			wgpuRenderPassEncoderSetBindGroup(pass, 1, group, 0, nullptr);
			wgpuRenderPassEncoderSetVertexBuffer(pass, 0, mesh.vertices, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderSetIndexBuffer(pass, mesh.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderDrawIndexed(pass, mesh.indexCount, 1, 0, 0, index);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}
} // namespace aether::webgpu
