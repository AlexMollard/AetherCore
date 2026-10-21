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
		if (m_pipeline)
		{
			wgpuRenderPipelineRelease(m_pipeline);
			m_pipeline = nullptr;
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
		std::array<WGPUVertexAttribute, 2> attributes{};
		attributes[0].format = WGPUVertexFormat_Float32x3;
		attributes[0].offset = offsetof(MeshVertex, position);
		attributes[0].shaderLocation = 0;
		attributes[1].format = WGPUVertexFormat_Float32x3;
		attributes[1].offset = offsetof(MeshVertex, color);
		attributes[1].shaderLocation = 1;
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
		m_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
		wgpuShaderModuleRelease(shader);
		return m_pipeline != nullptr;
	}

	bool WebMeshRenderer::Upload(WGPUDevice device, WGPUQueue queue, std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices)
	{
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
		for (const auto& vertex: vertices)
		{
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

	std::expected<std::vector<gpu::MeshGpuInstance>, std::string> WebMeshRenderer::Prepare(const RenderFramePacket& packet) const
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
	        std::unique_ptr<WebMeshFrame>& frame)
	{
		const auto clip = glm::transpose(packet.proj * packet.view);
		frame = std::make_unique<WebMeshFrame>();
		frame->uniform = Buffer(device, queue, WGPUBufferUsage_Uniform, &clip, sizeof(clip));
		frame->instances = Buffer(device, queue, WGPUBufferUsage_Storage, instances.data(), instances.size_bytes());
		WGPUTextureDescriptor depth = WGPU_TEXTURE_DESCRIPTOR_INIT;
		depth.size = {width, height, 1};
		depth.dimension = WGPUTextureDimension_2D;
		depth.format = WGPUTextureFormat_Depth32Float;
		depth.usage = WGPUTextureUsage_RenderAttachment;
		frame->depth = wgpuDeviceCreateTexture(device, &depth);
		frame->depthView = wgpuTextureCreateView(frame->depth, nullptr);
		std::array<WGPUBindGroupEntry, 2> entries{};
		entries[0].binding = 0;
		entries[0].buffer = frame->uniform;
		entries[0].size = sizeof(clip);
		entries[1].binding = 1;
		entries[1].buffer = frame->instances;
		entries[1].size = instances.size_bytes();
		const auto layout = wgpuRenderPipelineGetBindGroupLayout(m_pipeline, 0);
		WGPUBindGroupDescriptor bindings = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindings.layout = layout;
		bindings.entries = entries.data();
		bindings.entryCount = entries.size();
		frame->bindings = wgpuDeviceCreateBindGroup(device, &bindings);
		wgpuBindGroupLayoutRelease(layout);
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
		wgpuRenderPassEncoderSetPipeline(pass, m_pipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, frame->bindings, 0, nullptr);
		for (std::uint32_t index = 0; index < packet.render3D.meshes.size(); ++index)
		{
			const auto& mesh = m_meshes[packet.render3D.meshes[index].mesh];
			wgpuRenderPassEncoderSetVertexBuffer(pass, 0, mesh.vertices, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderSetIndexBuffer(pass, mesh.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderDrawIndexed(pass, mesh.indexCount, 1, 0, 0, index);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}
} // namespace aether::webgpu
