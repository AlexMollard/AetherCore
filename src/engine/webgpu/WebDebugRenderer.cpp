#include "WebDebugRenderer.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>
#include <glm/glm.hpp>
#include "physics/PhysicsDebugShared.hpp"
#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	namespace
	{
		WGPUStringView Text(const char* text)
		{
			return {text, std::strlen(text)};
		}

		constexpr const char* kShader = R"(
struct Constants { rows: array<vec4f, 4>, size: vec4f };
@group(0) @binding(0) var<uniform> constants: Constants;
@group(0) @binding(1) var sceneDepth: texture_depth_2d;
struct Output { @builtin(position) position: vec4f, @location(0) color: vec4f };
fn project(position: vec3f) -> vec4f {
    let p = vec4f(position, 1.0);
    // Render packets use Vulkan clip Y; WebGPU NDC has +Y up.
    return vec4f(dot(constants.rows[0], p), -dot(constants.rows[1], p), dot(constants.rows[2], p), dot(constants.rows[3], p));
}
@vertex fn vertexMain(@builtin(vertex_index) index: u32, @location(0) p0: vec3f, @location(1) c0: vec4f,
                      @location(2) p1: vec3f, @location(3) c1: vec4f) -> Output {
    var a = project(p0);
    var b = project(p1);
    var ca = c0;
    var cb = c1;
    var out: Output;
    if (a.z < 0.0 && b.z < 0.0) {
        out.position = vec4f(0.0, 0.0, -1.0, 1.0);
        out.color = ca;
        return out;
    }
    if (a.z < 0.0) {
        let t = -a.z / (b.z - a.z);
        a = mix(a, b, t);
        ca = mix(ca, cb, t);
    } else if (b.z < 0.0) {
        let t = -b.z / (a.z - b.z);
        b = mix(b, a, t);
        cb = mix(cb, ca, t);
    }
    let delta = (b.xy / max(b.w, 0.000001) - a.xy / max(a.w, 0.000001)) * constants.size.xy;
    let lengthSquared = dot(delta, delta);
    let normal = select(vec2f(0.0), vec2f(-delta.y, delta.x) * inverseSqrt(max(lengthSquared, 0.000001)), lengthSquared > 0.0);
    let ends = array<u32, 6>(0, 0, 1, 1, 0, 1);
    let sides = array<f32, 6>(-1.0, 1.0, -1.0, -1.0, 1.0, 1.0);
    let end = ends[index];
    out.position = select(a, b, end == 1u);
    out.position = vec4f(out.position.xy + normal * (2.0 / constants.size.xy) * sides[index] * out.position.w, out.position.zw);
    out.color = select(ca, cb, end == 1u);
    return out;
}
@fragment fn attachmentMain(input: Output) -> @location(0) vec4f { return input.color; }
@fragment fn sampledMain(input: Output) -> @location(0) vec4f {
    let dims = textureDimensions(sceneDepth);
    let pixel = min(vec2u(input.position.xy / constants.size.xy * vec2f(dims)), dims - vec2u(1));
    if (input.position.z > textureLoad(sceneDepth, vec2i(pixel), 0)) { discard; }
    return input.color;
}
)";
	} // namespace

	WebDebugFrame::~WebDebugFrame()
	{
		if (binding)
		{
			wgpuBindGroupRelease(binding);
		}
		if (vertices)
		{
			wgpuBufferDestroy(vertices);
			wgpuBufferRelease(vertices);
		}
		if (uniform)
		{
			wgpuBufferDestroy(uniform);
			wgpuBufferRelease(uniform);
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

	WebDebugRenderer::~WebDebugRenderer()
	{
		Shutdown();
	}

	bool WebDebugRenderer::Initialize(WGPUDevice device)
	{
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = Text(kShader);
		WGPUShaderModuleDescriptor shaderDesc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shaderDesc.nextInChain = &source.chain;
		const auto shader = wgpuDeviceCreateShaderModule(device, &shaderDesc);
		if (!shader)
		{
			error = "Debug shader creation failed";
			return false;
		}
		std::array<WGPUVertexAttribute, 4> attrs{};
		attrs[0] = WGPU_VERTEX_ATTRIBUTE_INIT;
		attrs[0].format = WGPUVertexFormat_Float32x3;
		attrs[0].offset = offsetof(DebugVertex, position);
		attrs[0].shaderLocation = 0;
		attrs[1] = WGPU_VERTEX_ATTRIBUTE_INIT;
		attrs[1].format = WGPUVertexFormat_Float32x4;
		attrs[1].offset = offsetof(DebugVertex, color);
		attrs[1].shaderLocation = 1;
		attrs[2] = WGPU_VERTEX_ATTRIBUTE_INIT;
		attrs[2].format = WGPUVertexFormat_Float32x3;
		attrs[2].offset = sizeof(DebugVertex) + offsetof(DebugVertex, position);
		attrs[2].shaderLocation = 2;
		attrs[3] = WGPU_VERTEX_ATTRIBUTE_INIT;
		attrs[3].format = WGPUVertexFormat_Float32x4;
		attrs[3].offset = sizeof(DebugVertex) + offsetof(DebugVertex, color);
		attrs[3].shaderLocation = 3;
		WGPUVertexBufferLayout vertex = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
		vertex.arrayStride = 2 * sizeof(DebugVertex);
		vertex.stepMode = WGPUVertexStepMode_Instance;
		vertex.attributeCount = attrs.size();
		vertex.attributes = attrs.data();
		WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
		blend.color.operation = blend.alpha.operation = WGPUBlendOperation_Add;
		blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
		blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
		target.format = WGPUTextureFormat_RGBA8Unorm;
		target.blend = &blend;
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
		fragment.module = shader;
		fragment.targetCount = 1;
		fragment.targets = &target;
		WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
		depth.format = WGPUTextureFormat_Depth32Float;
		depth.depthWriteEnabled = WGPUOptionalBool_False;
		depth.depthCompare = WGPUCompareFunction_LessEqual;
		WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
		desc.vertex.module = shader;
		desc.vertex.entryPoint = Text("vertexMain");
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &vertex;
		desc.fragment = &fragment;
		desc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		fragment.entryPoint = Text("attachmentMain");
		desc.depthStencil = &depth;
		m_attachmentPipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
		fragment.entryPoint = Text("sampledMain");
		desc.depthStencil = nullptr;
		m_sampledPipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
		wgpuShaderModuleRelease(shader);
		if (!m_attachmentPipeline || !m_sampledPipeline)
		{
			error = "Debug pipeline creation failed";
			Shutdown();
			return false;
		}
		return true;
	}

	void WebDebugRenderer::Shutdown()
	{
		if (m_attachmentPipeline)
		{
			wgpuRenderPipelineRelease(m_attachmentPipeline);
		}
		if (m_sampledPipeline)
		{
			wgpuRenderPipelineRelease(m_sampledPipeline);
		}
		m_attachmentPipeline = m_sampledPipeline = nullptr;
	}

	bool WebDebugRenderer::Encode(
	        WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, WGPUTextureView color, unsigned width, unsigned height, const RenderFramePacket& packet, WGPUTextureView depth, bool sampledDepth, std::unique_ptr<WebDebugFrame>& frame)
	{
		if (!packet.debugRenderingEnabled || (packet.debugVertices.empty() && packet.physicsDebugShapes.empty()))
		{
			return true;
		}
		const auto immediateCount = packet.debugVertices.size() & ~std::size_t(1);
		std::size_t vertexCount = immediateCount;
		for (const auto& shape: packet.physicsDebugShapes)
		{
			vertexCount += GetPhysicsDebugGeometry(shape.shape).size();
		}
		std::vector<DebugVertex> vertices;
		vertices.reserve(vertexCount);
		vertices.insert(vertices.end(), packet.debugVertices.begin(), packet.debugVertices.begin() + immediateCount);
		for (const auto& shape: packet.physicsDebugShapes)
		{
			const auto& geometry = GetPhysicsDebugGeometry(shape.shape);
			for (const auto& v: geometry)
			{
				const auto p = shape.model * glm::vec4(v.position, 1.0f);
				vertices.push_back({glm::vec3(p), glm::vec4(glm::vec3(v.color) * glm::vec3(shape.tint), v.color.a)});
			}
		}
		if (vertices.empty())
		{
			return true;
		}
		frame = std::make_unique<WebDebugFrame>();
		const auto makeBuffer = [&](WGPUBufferUsage usage, const void* data, size_t size)
		{
			WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
			desc.size = size;
			desc.usage = usage | WGPUBufferUsage_CopyDst;
			auto buffer = wgpuDeviceCreateBuffer(device, &desc);
			if (buffer)
			{
				wgpuQueueWriteBuffer(queue, buffer, 0, data, size);
			}
			return buffer;
		};
		frame->vertices = makeBuffer(WGPUBufferUsage_Vertex, vertices.data(), vertices.size() * sizeof(DebugVertex));
		const auto clip = glm::transpose(packet.proj * packet.view);
		const std::array<glm::vec4, 5> constants{clip[0], clip[1], clip[2], clip[3], glm::vec4(width, height, 0, 0)};
		frame->uniform = makeBuffer(WGPUBufferUsage_Uniform, constants.data(), sizeof(constants));
		if (!frame->vertices || !frame->uniform)
		{
			error = "Debug buffer creation failed";
			return false;
		}
		if (!depth)
		{
			WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
			desc.size = {width, height, 1};
			desc.dimension = WGPUTextureDimension_2D;
			desc.format = WGPUTextureFormat_Depth32Float;
			desc.usage = WGPUTextureUsage_RenderAttachment;
			frame->depth = wgpuDeviceCreateTexture(device, &desc);
			if (frame->depth)
			{
				frame->depthView = wgpuTextureCreateView(frame->depth, nullptr);
			}
			if (!frame->depthView)
			{
				error = "Debug depth creation failed";
				return false;
			}
			depth = frame->depthView;
			sampledDepth = false;
		}
		const auto pipeline = sampledDepth ? m_sampledPipeline : m_attachmentPipeline;
		const auto layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
		std::array<WGPUBindGroupEntry, 2> entries{};
		entries[0] = WGPU_BIND_GROUP_ENTRY_INIT;
		entries[0].binding = 0;
		entries[0].buffer = frame->uniform;
		entries[0].size = sizeof(constants);
		entries[1] = WGPU_BIND_GROUP_ENTRY_INIT;
		entries[1].binding = 1;
		entries[1].textureView = depth;
		WGPUBindGroupDescriptor group = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		group.layout = layout;
		group.entryCount = sampledDepth ? 2 : 1;
		group.entries = entries.data();
		frame->binding = wgpuDeviceCreateBindGroup(device, &group);
		wgpuBindGroupLayoutRelease(layout);
		if (!frame->binding)
		{
			error = "Debug bind group creation failed";
			return false;
		}
		WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		attachment.view = color;
		attachment.loadOp = WGPULoadOp_Load;
		attachment.storeOp = WGPUStoreOp_Store;
		WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		depthAttachment.view = depth;
		depthAttachment.depthLoadOp = frame->depth ? WGPULoadOp_Clear : WGPULoadOp_Load;
		depthAttachment.depthStoreOp = WGPUStoreOp_Store;
		depthAttachment.depthClearValue = 1;
		WGPURenderPassDescriptor passDesc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &attachment;
		passDesc.depthStencilAttachment = sampledDepth ? nullptr : &depthAttachment;
		const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDesc);
		wgpuRenderPassEncoderSetPipeline(pass, pipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, frame->binding, 0, nullptr);
		wgpuRenderPassEncoderSetVertexBuffer(pass, 0, frame->vertices, 0, vertices.size() * sizeof(DebugVertex));
		wgpuRenderPassEncoderDraw(pass, 6, static_cast<uint32_t>(vertices.size() / 2), 0, 0);
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
		return true;
	}
} // namespace aether::webgpu
