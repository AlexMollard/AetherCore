#include "WebMeshRenderer.hpp"

#include <array>
#include <ShadowCascadeFit.hpp>
#include <cmath>
#include <fstream>
#include <iterator>
#include <glm/gtc/matrix_transform.hpp>

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

	WebShadowTargets::~WebShadowTargets()
	{
		if (sample)
		{
			wgpuTextureViewRelease(sample);
		}
		if (color)
		{
			wgpuTextureDestroy(color);
			wgpuTextureRelease(color);
		}
		if (depth)
		{
			wgpuTextureDestroy(depth);
			wgpuTextureRelease(depth);
		}
	}

	WebMeshFrame::~WebMeshFrame()
	{
		if (bindings)
		{
			wgpuBindGroupRelease(bindings);
		}
		Destroy(uniform);
		Destroy(instances);
		Destroy(skin);
		Destroy(lights);
		Destroy(shadows);
		for (const auto group: shadowBindings)
		{
			wgpuBindGroupRelease(group);
		}
		for (const auto buffer: shadowUniforms)
		{
			Destroy(buffer);
		}

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
		m_shadow_targets.reset();
		m_shadow_fallback.reset();
		if (m_shadow_pipeline)
		{
			wgpuRenderPipelineRelease(m_shadow_pipeline);
			m_shadow_pipeline = nullptr;
		}
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
		if (m_shadow_frame_layout)
		{
			wgpuBindGroupLayoutRelease(m_shadow_frame_layout);
			m_shadow_frame_layout = nullptr;
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
		WGPULimits limits = WGPU_LIMITS_INIT;
		wgpuDeviceGetLimits(device, &limits);
		m_max_texture_dimension = limits.maxTextureDimension2D;
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
		std::array<WGPUVertexAttribute, 7> attributes{};
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
		attributes[5].format = WGPUVertexFormat_Float32x3;
		attributes[5].offset = offsetof(MeshVertex, normal);
		attributes[5].shaderLocation = 5;
		attributes[6].format = WGPUVertexFormat_Float32x4;
		attributes[6].offset = offsetof(MeshVertex, tangent);
		attributes[6].shaderLocation = 6;
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
		std::array<WGPUBindGroupLayoutEntry, 6> frameEntries{};
		for (unsigned i = 0; i < 5; ++i)
		{
			frameEntries[i].binding = i;
			frameEntries[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
			frameEntries[i].buffer.type = i == 0 ? WGPUBufferBindingType_Uniform : WGPUBufferBindingType_ReadOnlyStorage;
		}
		frameEntries[5].binding = 5;
		frameEntries[5].visibility = WGPUShaderStage_Fragment;
		frameEntries[5].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
		frameEntries[5].texture.viewDimension = WGPUTextureViewDimension_2DArray;
		WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.entryCount = frameEntries.size();
		layoutDescriptor.entries = frameEntries.data();
		m_frame_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		layoutDescriptor.entryCount = 3;
		m_shadow_frame_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		std::array<WGPUBindGroupLayoutEntry, 6> materialEntries{};
		for (unsigned i = 0; i < 6; ++i)
		{
			if (i == 2)
			{
				continue;
			}
			materialEntries[i].binding = i;
			materialEntries[i].visibility = WGPUShaderStage_Fragment;
			materialEntries[i].texture.sampleType = WGPUTextureSampleType_Float;
			materialEntries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		materialEntries[2].binding = 2;
		materialEntries[2].visibility = WGPUShaderStage_Fragment;
		materialEntries[2].sampler.type = WGPUSamplerBindingType_Filtering;
		layoutDescriptor.entryCount = materialEntries.size();
		layoutDescriptor.entries = materialEntries.data();
		m_material_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		std::array<WGPUBindGroupLayout, 2> layouts{m_frame_layout, m_material_layout};
		WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pipelineLayoutDescriptor.bindGroupLayoutCount = layouts.size();
		pipelineLayoutDescriptor.bindGroupLayouts = layouts.data();
		const auto pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
		descriptor.layout = pipelineLayout;
		for (unsigned target = 0; target < 2; ++target)
		{
			color.format = target ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
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
					m_pipelines[target * 9 + blend * 3 + winding] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
				}
			}
		}
		fragment.entryPoint = {"shadowMain", 10};
		color.format = WGPUTextureFormat_R32Float;
		color.blend = nullptr;
		fragment.targetCount = 1;
		fragment.targets = &color;
		layouts[0] = m_shadow_frame_layout;
		const auto shadowLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
		descriptor.layout = shadowLayout;
		depth.depthWriteEnabled = WGPUOptionalBool_True;
		descriptor.primitive.cullMode = WGPUCullMode_None;
		m_shadow_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
		wgpuPipelineLayoutRelease(shadowLayout);
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
		return m_white && m_sampler && m_shadow_pipeline && std::all_of(m_pipelines.begin(), m_pipelines.end(), [](auto pipeline) { return pipeline != nullptr; });
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
			// All consumed vertex attributes are validated before GPU allocation.
			if (!std::isfinite(vertex.tangent.w) || !std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y))
			{
				error = "Nonfinite mesh UV";
				return false;
			}
			for (int axis = 0; axis < 3; ++axis)
			{
				if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.color[axis]) || !std::isfinite(vertex.normal[axis]) || !std::isfinite(vertex.tangent[axis]))
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
		if (packet.pointLights.size() + packet.spotLights.size() > 128)
		{
			return std::unexpected("WebGPU local light limit exceeded (128)");
		}
		std::size_t shadowMaps = packet.directionalShadowEnabled ? 3 : 0;
		for (const auto& light: packet.pointLights)
		{
			shadowMaps += light.castsShadow ? 6 : 0;
		}
		for (const auto& light: packet.spotLights)
		{
			shadowMaps += light.castsShadow ? 1 : 0;
		}
		if (shadowMaps > 16)
		{
			return std::unexpected("WebGPU shadow map limit exceeded (16 layers)");
		}
		if (shadowMaps && m_max_texture_dimension < 7168)
		{
			return std::unexpected("WebGPU shadow atlas requires maxTextureDimension2D >= 7168");
		}
		const auto finite = [](glm::vec4 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		};
		for (const auto value: {packet.sunDirectionIntensity, packet.sunColor, packet.ambientColor, packet.skyHorizonColor, packet.skyZenithColor, packet.skyVoidColor, packet.skyParams, packet.shadingParams, packet.cameraWorldPos})
		{
			if (!finite(value))
			{
				return std::unexpected("Nonfinite mesh lighting constants");
			}
		}
		for (const auto& light: packet.pointLights)
		{
			if (!finite(glm::vec4(light.position, light.radius)) || !finite(glm::vec4(light.color, light.intensity)) || !std::isfinite(light.sourceRadius) || light.sourceRadius < 0 || light.radius <= 0 || (light.castsShadow && light.radius <= 0.1f)
			        || light.intensity < 0)
			{
				return std::unexpected("Invalid point light position, radius, color or intensity");
			}
		}
		for (const auto& light: packet.spotLights)
		{
			if (!finite(glm::vec4(light.position, light.radius)) || !finite(glm::vec4(light.color, light.intensity)) || !finite(glm::vec4(light.direction, light.innerAngleRad)) || !std::isfinite(light.outerAngleRad)
			        || !std::isfinite(light.sourceRadius) || light.sourceRadius < 0 || light.radius <= 0 || (light.castsShadow && light.radius <= 0.1f) || light.intensity < 0 || glm::dot(light.direction, light.direction) < 1e-12f
			        || light.innerAngleRad < 0 || light.innerAngleRad > light.outerAngleRad || light.outerAngleRad >= 1.5707963f)
			{
				return std::unexpected("Invalid spot light position, radius, direction or cone");
			}
		}
		if (shadowMaps != 0)
		{
			const auto farClip = glm::inverse(packet.proj) * glm::vec4(0, 0, 1, 1);
			if (!std::isfinite(glm::determinant(packet.view)) || std::abs(glm::determinant(packet.view)) < 1e-12f || !finite(farClip) || std::abs(farClip.w) < 1e-12f || !std::isfinite(packet.cameraNearPlane) || packet.cameraNearPlane <= 0
			        || std::abs(farClip.z / farClip.w) <= packet.cameraNearPlane || !std::isfinite(packet.shadowSplitLambda))
			{
				return std::unexpected("Invalid shadow camera projection, range or cascade split");
			}
			if (packet.directionalShadowEnabled && (packet.cameraNearPlane >= std::min(std::abs(farClip.z / farClip.w), 320.0f) || std::abs(packet.proj[0][0]) < 1e-12f || std::abs(packet.proj[1][1]) < 1e-12f))
			{
				return std::unexpected("Directional shadow camera near plane must lie inside the 320-unit shadow range");
			}
		}
		const auto instances = gpu::PlanMeshPass(packet.render3D);
		if (!instances)
		{
			return std::unexpected(instances.error());
		}
		if (packet.renderLightingEnabled)
		{
			constexpr std::uint32_t kSupportedFlags = 1 | 2 | 4 | 8 | 16 | 256 | gpu::kMeshMirrored;
			for (const auto& instance: *instances)
			{
				if ((instance.metadata.w & ~kSupportedFlags) != 0)
				{
					return std::unexpected("WebGPU lit mesh uses a specialized material effect scheduled for parity Task 4; use a standard PBR material");
				}
			}
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
	        std::unique_ptr<WebMeshFrame>& frame,
	        bool hdr)
	{
		const auto clip = glm::transpose(packet.proj * packet.view);
		frame = std::make_unique<WebMeshFrame>();

		struct FrameConstants
		{
			glm::vec4 clip0, clip1, clip2, clip3, time, camera, sun, sunColor, ambient;
			glm::vec4 horizon, zenith, ground, skyParams, shadingParams;
			glm::uvec4 counts;
			glm::vec4 viewDepth, cascadeSplits;
		};

		struct ShadowData
		{
			glm::vec4 c0, c1, c2, c3, params, lightPosRange, region;
		};

		std::vector<ShadowData> shadows;
		glm::vec4 splits(0);
		auto appendShadow = [&](glm::mat4 matrix, glm::vec4 params, glm::vec4 lightPosRange = glm::vec4(0), unsigned resolution = 512)
		{
			const auto index = unsigned(shadows.size());
			glm::vec4 region;
			if (packet.directionalShadowEnabled && index < 3)
			{
				region = index == 0 ? glm::vec4(0, 0, 4096, 4096) : glm::vec4((index - 1) * 2048, 4096, 2048, 2048);
			}
			else
			{
				const auto tile = index - (packet.directionalShadowEnabled ? 3 : 0);
				region = glm::vec4((tile % 8) * 512, 6144 + (tile / 8) * 512, resolution, resolution);
			}
			shadows.push_back({matrix[0], matrix[1], matrix[2], matrix[3], params, lightPosRange, region});
		};
		if (hdr && packet.directionalShadowEnabled)
		{
			const float nearZ = std::max(packet.cameraNearPlane, 0.001f);
			const auto farClip = glm::inverse(packet.proj) * glm::vec4(0, 0, 1, 1);
			const float farZ = std::min(std::abs(farClip.z / farClip.w), 320.0f);
			const float range = std::max(farZ - nearZ, 1.0f);
			for (int i = 0; i < 3; ++i)
			{
				const float fraction = float(i + 1) / 3;
				splits[i] = glm::mix(nearZ + range * fraction, nearZ * std::pow(farZ / nearZ, fraction), std::clamp(packet.shadowSplitLambda, 0.0f, 1.0f));
			}
			splits.z = farZ;
			auto lightDir = glm::vec3(packet.sunDirectionIntensity);
			lightDir = glm::length(lightDir) < 1e-4f ? glm::normalize(glm::vec3(0.5f, 0.8f, 0.2f)) : glm::normalize(lightDir);
			const auto forward = glm::normalize(glm::vec3(glm::inverse(packet.view) * glm::vec4(0, 0, -1, 0)));
			for (int i = 0; i < 3; ++i)
			{
				const float n = i == 0 ? nearZ : splits[i - 1], f = splits[i];
				const float x = 1 / std::abs(packet.proj[0][0]), y = 1 / std::abs(packet.proj[1][1]);
				const auto sphere = FitShadowCascadeSphere(n, f, x, y, std::abs(packet.proj[3][3] - 1.0f) < 1e-6f);
				const float half = std::max(20.0f, sphere.radius) * 1.04f;
				const float texel = 2 * half / (i == 0 ? 4096 : 2048);
				const auto up = std::abs(lightDir.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
				const auto basis = glm::lookAt(glm::vec3(0), -lightDir, up);
				auto center = glm::vec3(packet.cameraWorldPos) + forward * sphere.centerDistance;
				auto centerLs = glm::vec3(basis * glm::vec4(center, 1));
				centerLs.x = std::floor(centerLs.x / texel + 0.5f) * texel;
				centerLs.y = std::floor(centerLs.y / texel + 0.5f) * texel;
				center = glm::vec3(glm::inverse(basis) * glm::vec4(centerLs, 1));
				const float nearPlane = std::max(0.1f, n * 0.5f), farPlane = std::max(nearPlane + 64, f + std::max(f - n, 1.0f) + 140);
				const auto matrix = glm::ortho(-half, half, -half, half, nearPlane, farPlane) * glm::lookAt(center + lightDir * (f + 120), center, up);
				appendShadow(matrix, glm::vec4(1.5f * texel * 1.41421356f / (farPlane - nearPlane), 6 * texel, (farPlane - nearPlane) * 0.00465f / (2 * half), 0));
			}
		}

		struct LightData
		{
			glm::vec4 positionRadius, colorIntensity, directionType, coneShadow;
		};

		std::vector<LightData> lights;
		constexpr std::array<glm::vec3, 6> directions{glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)};
		constexpr std::array<glm::vec3, 6> ups{glm::vec3(0, -1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::vec3(0, -1, 0), glm::vec3(0, -1, 0)};
		for (const auto& light: packet.pointLights)
		{
			const float layer = hdr && light.castsShadow ? float(shadows.size()) : -1;
			lights.push_back({glm::vec4(light.position, light.radius), glm::vec4(light.color, light.intensity), glm::vec4(0, -1, 0, 0), glm::vec4(0, 0, layer, 0)});
			if (layer >= 0)
			{
				for (int face = 0; face < 6; ++face)
				{
					appendShadow(glm::perspective(2.0f * std::atan(396.0f / 384.0f), 1.0f, 0.1f, light.radius) * glm::lookAt(light.position, light.position + directions[face], ups[face]),
					        glm::vec4(0, 2.0f * (396.0f / 384.0f) / 384.0f, light.sourceRadius, 1),
					        glm::vec4(light.position, light.radius),
					        384);
				}
			}
		}
		for (const auto& light: packet.spotLights)
		{
			const float layer = hdr && light.castsShadow ? float(shadows.size()) : -1;
			const auto direction = glm::normalize(light.direction);
			lights.push_back({glm::vec4(light.position, light.radius), glm::vec4(light.color, light.intensity), glm::vec4(direction, 1), glm::vec4(std::cos(light.innerAngleRad), std::cos(light.outerAngleRad), layer, 0)});
			if (layer >= 0)
			{
				const auto up = std::abs(direction.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
				appendShadow(glm::perspective(std::min(2 * (light.outerAngleRad + glm::radians(4.0f)), glm::radians(175.0f)), 1.0f, 0.1f, light.radius) * glm::lookAt(light.position, light.position + direction, up),
				        glm::vec4(0, 2.0f * std::tan(std::min(light.outerAngleRad + glm::radians(4.0f), glm::radians(87.5f))) / 512, light.sourceRadius, 1),
				        glm::vec4(light.position, light.radius));
			}
		}
		const auto viewRows = glm::transpose(packet.view);
		const FrameConstants constants{clip[0],
		        clip[1],
		        clip[2],
		        clip[3],
		        glm::vec4(packet.elapsedTime, hdr ? 1 : 0, 0, 0),
		        packet.cameraWorldPos,
		        packet.sunDirectionIntensity,
		        packet.sunColor,
		        packet.ambientColor,
		        packet.skyHorizonColor,
		        packet.skyZenithColor,
		        packet.skyVoidColor,
		        packet.skyParams,
		        packet.shadingParams,
		        glm::uvec4(lights.size(), hdr && packet.directionalShadowEnabled ? 1 : 0, 0, 0),
		        -viewRows[2],
		        splits};
		const auto shadowCount = shadows.size();
		if (shadows.empty())
		{
			shadows.emplace_back();
		}
		frame->shadows = Buffer(device, queue, WGPUBufferUsage_Storage, shadows.data(), shadows.size() * sizeof(ShadowData));
		// Queue submissions execute in order, so frames can reuse the atlas while
		// retaining its ownership until their own completion callback retires.
		auto& cache = shadowCount ? m_shadow_targets : m_shadow_fallback;
		if (!cache)
		{
			cache = std::make_shared<WebShadowTargets>();
			WGPUTextureDescriptor texture = WGPU_TEXTURE_DESCRIPTOR_INIT;
			texture.size = shadowCount ? WGPUExtent3D{4096, 7168, 1} : WGPUExtent3D{1, 1, 1};
			texture.dimension = WGPUTextureDimension_2D;
			texture.format = WGPUTextureFormat_R32Float;
			texture.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
			cache->color = wgpuDeviceCreateTexture(device, &texture);
			texture.format = WGPUTextureFormat_Depth32Float;
			texture.usage = WGPUTextureUsage_RenderAttachment;
			cache->depth = wgpuDeviceCreateTexture(device, &texture);
			WGPUTextureViewDescriptor view = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
			view.dimension = WGPUTextureViewDimension_2DArray;
			view.arrayLayerCount = 1;
			cache->sample = wgpuTextureCreateView(cache->color, &view);
		}
		frame->shadowTargets = cache;
		if (lights.empty())
		{
			lights.emplace_back();
		}
		frame->lights = Buffer(device, queue, WGPUBufferUsage_Storage, lights.data(), lights.size() * sizeof(LightData));
		frame->uniform = Buffer(device, queue, WGPUBufferUsage_Uniform, &constants, sizeof(constants));
		auto materialInstances = std::vector<gpu::MeshGpuInstance>(instances.begin(), instances.end());
		for (std::size_t i = 0; i < materialInstances.size(); ++i)
		{
			const std::uint32_t textureFlags = packet.render3D.meshes[i].textures[1] == UINT32_MAX ? 0 : 2;
			materialInstances[i].factors.w = static_cast<float>(textureFlags);
		}
		frame->instances = Buffer(device, queue, WGPUBufferUsage_Storage, materialInstances.data(), instances.size_bytes());
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
		std::array<WGPUBindGroupEntry, 6> entries{};
		entries[0].binding = 0;
		entries[0].buffer = frame->uniform;
		entries[0].size = sizeof(constants);
		entries[1].binding = 1;
		entries[1].buffer = frame->instances;
		entries[1].size = instances.size_bytes();
		entries[2].binding = 2;
		entries[2].buffer = frame->skin;
		entries[2].size = palette.size() * sizeof(glm::mat4);
		entries[3].binding = 3;
		entries[3].buffer = frame->lights;
		entries[3].size = lights.size() * sizeof(LightData);
		entries[4].binding = 4;
		entries[4].buffer = frame->shadows;
		entries[4].size = shadows.size() * sizeof(ShadowData);
		entries[5].binding = 5;
		entries[5].textureView = frame->shadowTargets->sample;
		WGPUBindGroupDescriptor bindings = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindings.layout = m_frame_layout;
		bindings.entries = entries.data();
		bindings.entryCount = entries.size();
		frame->bindings = wgpuDeviceCreateBindGroup(device, &bindings);
		for (const auto& instance: packet.render3D.meshes)
		{
			std::array<WGPUBindGroupEntry, 6> materialEntries{};
			std::array<WGPUTextureView, 5> views{};
			constexpr std::array<unsigned, 5> textureIndices{0, 4, 1, 2, 3};
			constexpr std::array<unsigned, 5> bindingIndices{0, 1, 3, 4, 5};
			for (unsigned i = 0; i < 5; ++i)
			{
				const auto slot = instance.textures[textureIndices[i]];
				views[i] = wgpuTextureCreateView(slot == UINT32_MAX ? m_white : textures[slot], nullptr);
				materialEntries[bindingIndices[i]].binding = bindingIndices[i];
				materialEntries[bindingIndices[i]].textureView = views[i];
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
		}
		for (unsigned layer = 0; layer < shadowCount; ++layer)
		{
			auto shadowConstants = constants;
			const auto& data = shadows[layer];
			const auto matrix = glm::transpose(glm::mat4(data.c0, data.c1, data.c2, data.c3));
			shadowConstants.camera = data.lightPosRange;
			shadowConstants.clip0 = matrix[0];
			shadowConstants.clip1 = matrix[1];
			shadowConstants.clip2 = matrix[2];
			shadowConstants.clip3 = matrix[3];
			const auto uniform = Buffer(device, queue, WGPUBufferUsage_Uniform, &shadowConstants, sizeof(shadowConstants));
			frame->shadowUniforms.push_back(uniform);
			entries[0].buffer = uniform;
			bindings.layout = m_shadow_frame_layout;
			bindings.entryCount = 3;
			const auto group = wgpuDeviceCreateBindGroup(device, &bindings);
			frame->shadowBindings.push_back(group);
			WGPUTextureViewDescriptor viewDescriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
			viewDescriptor.dimension = WGPUTextureViewDimension_2D;
			viewDescriptor.baseArrayLayer = 0;
			viewDescriptor.arrayLayerCount = 1;
			const auto view = wgpuTextureCreateView(frame->shadowTargets->color, &viewDescriptor);
			WGPURenderPassDepthStencilAttachment attachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
			const auto zView = wgpuTextureCreateView(frame->shadowTargets->depth, &viewDescriptor);
			attachment.view = zView;
			attachment.depthLoadOp = layer == 0 ? WGPULoadOp_Clear : WGPULoadOp_Load;
			attachment.depthStoreOp = WGPUStoreOp_Store;
			attachment.depthClearValue = 1;
			WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			passDescriptor.depthStencilAttachment = &attachment;
			WGPURenderPassColorAttachment shadowColor = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			shadowColor.view = view;
			shadowColor.loadOp = layer == 0 ? WGPULoadOp_Clear : WGPULoadOp_Load;
			shadowColor.storeOp = WGPUStoreOp_Store;
			shadowColor.clearValue = {1, 1, 1, 1};
			passDescriptor.colorAttachmentCount = 1;
			passDescriptor.colorAttachments = &shadowColor;
			const auto shadowPass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
			wgpuRenderPassEncoderSetPipeline(shadowPass, m_shadow_pipeline);
			wgpuRenderPassEncoderSetViewport(shadowPass, data.region.x, data.region.y, data.region.z, data.region.w, 0, 1);
			wgpuRenderPassEncoderSetScissorRect(shadowPass, unsigned(data.region.x), unsigned(data.region.y), unsigned(data.region.z), unsigned(data.region.w));
			wgpuRenderPassEncoderSetBindGroup(shadowPass, 0, group, 0, nullptr);
			for (unsigned i = 0; i < packet.render3D.meshes.size(); ++i)
			{
				const auto& instance = packet.render3D.meshes[i];
				if (!instance.castShadows || instance.material.alphaBlend)
				{
					continue;
				}
				const auto& mesh = m_meshes[instance.mesh];
				wgpuRenderPassEncoderSetBindGroup(shadowPass, 1, frame->materials[i], 0, nullptr);
				wgpuRenderPassEncoderSetVertexBuffer(shadowPass, 0, mesh.vertices, 0, WGPU_WHOLE_SIZE);
				wgpuRenderPassEncoderSetIndexBuffer(shadowPass, mesh.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
				wgpuRenderPassEncoderDrawIndexed(shadowPass, mesh.indexCount, 1, 0, 0, i);
			}
			wgpuRenderPassEncoderEnd(shadowPass);
			wgpuRenderPassEncoderRelease(shadowPass);
			wgpuTextureViewRelease(view);
			wgpuTextureViewRelease(zView);
		}
		WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		depthAttachment.view = frame->depthView;
		depthAttachment.depthLoadOp = WGPULoadOp_Clear;
		depthAttachment.depthStoreOp = WGPUStoreOp_Store;
		depthAttachment.depthClearValue = 1;
		WGPURenderPassColorAttachment colorAttachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		colorAttachment.view = color;
		colorAttachment.loadOp = hdr ? WGPULoadOp_Load : WGPULoadOp_Clear;
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
			wgpuRenderPassEncoderSetPipeline(pass, m_pipelines[(hdr ? 9 : 0) + blend * 3 + winding]);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, frame->bindings, 0, nullptr);
			wgpuRenderPassEncoderSetBindGroup(pass, 1, frame->materials[index], 0, nullptr);
			wgpuRenderPassEncoderSetVertexBuffer(pass, 0, mesh.vertices, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderSetIndexBuffer(pass, mesh.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderDrawIndexed(pass, mesh.indexCount, 1, 0, 0, index);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}
} // namespace aether::webgpu
