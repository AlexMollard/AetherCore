#include "WebMeshRenderer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <array>
#include <ShadowCascadeFit.hpp>
#include <LocalShadowPlan.hpp>
#include <cmath>
#include <fstream>
#include <iterator>
#include <glm/gtc/matrix_transform.hpp>

#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	namespace
	{

		struct ShadowPlan
		{
			std::vector<LocalShadowCandidate> selected;
			std::vector<glm::vec4> regions;
			unsigned width = 1, height = 1;
		};

		ShadowPlan PlanShadows(const RenderFramePacket& packet, unsigned maxDimension)
		{
			ShadowPlan plan;
			plan.selected = SelectLocalShadows(LocalShadowCandidates(packet.pointLights, packet.spotLights, glm::vec3(packet.cameraWorldPos)));
			std::vector<unsigned> sizes;
			if (packet.directionalShadowEnabled) { sizes = {4096, 2048, 2048}; }
			for (const auto& candidate: plan.selected)
			{
				for (unsigned i = 0; i < (candidate.lightType ? 1u : 6u); ++i) { sizes.push_back(candidate.lightType ? kLocalSpotShadowResolution : kLocalPointShadowResolution); }
			}
			auto atlas = PackShadowAtlas(sizes, maxDimension);
			plan.regions = std::move(atlas.regions);
			plan.width = atlas.width;
			plan.height = atlas.height;
			return plan;
		}

		WGPUBuffer Buffer(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* data, std::size_t bytes)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = bytes;
			descriptor.usage = usage | WGPUBufferUsage_CopyDst;
			const auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
			if (buffer && data)
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

	struct WebMeshPreparedData
	{
		struct FrameConstants
		{
			glm::vec4 clip0, clip1, clip2, clip3, time, camera, sun, sunColor, ambient;
			glm::vec4 horizon, zenith, ground, skyParams, shadingParams;
			glm::uvec4 counts;
			glm::vec4 viewDepth, cascadeSplits;
			glm::vec4 fogParams, objectAmbient, objectLight0Direction, objectLight0Color, objectLight1Direction, objectLight1Color;
			glm::vec4 invClip0, invClip1, invClip2, invClip3;
			glm::uvec4 volumeCounts{};
			std::array<glm::vec4, 64> blobs{};
			std::array<glm::vec4, 32> darkness{};
			glm::vec4 visibilityCamera;
			std::array<glm::vec4, 8> grassBenders{};
			std::array<glm::vec4, 8> grassBenderParams{};
		};

		struct ShadowData
		{
			glm::vec4 c0, c1, c2, c3, params, lightPosRange, region;
		};

		FrameConstants constants{};
		std::vector<ShadowData> shadows;
		std::vector<bool> cameraVisible;
		std::vector<bool> distanceVisible;
		std::size_t shadowCount = 0;
	};

	WebShadowTargets::~WebShadowTargets()
	{
		if (sceneSample) { wgpuTextureViewRelease(sceneSample); }
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
		if (auxiliaryBindings) { wgpuBindGroupRelease(auxiliaryBindings); }
		if (inputBindings) { wgpuBindGroupRelease(inputBindings); }
		if (tileCullBindings) { wgpuBindGroupRelease(tileCullBindings); }
		Destroy(tileCullUniform);
		Destroy(lightTiles);
		Destroy(uniform);
		Destroy(instances);
		Destroy(skin);
		Destroy(lights);
		Destroy(shadows);
		Destroy(effects);
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
			if (material) { wgpuBindGroupRelease(material); }
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
		for (auto& material: m_custom_pipelines)
		{
			for (auto pipeline: material) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); } }
		}
		m_custom_pipelines.clear();
		for (const auto& variants: m_custom_auxiliary)
		{
			for (auto pipeline: variants) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); } }
		}
		m_custom_auxiliary.clear();
		m_bytes = 0;
		m_shadow_targets.reset();
		m_shadow_fallback.reset();
		if (m_tile_cull_pipeline) { wgpuComputePipelineRelease(m_tile_cull_pipeline); m_tile_cull_pipeline = nullptr; }
		if (m_tile_cull_layout) { wgpuBindGroupLayoutRelease(m_tile_cull_layout); m_tile_cull_layout = nullptr; }
		if (m_auxiliary_pipeline)
		{
			wgpuRenderPipelineRelease(m_auxiliary_pipeline);
			m_auxiliary_pipeline = nullptr;
		}
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
		if (m_input_layout)
		{
			wgpuBindGroupLayoutRelease(m_input_layout);
			m_input_layout = nullptr;
		}
		if (m_scene_sampler)
		{
			wgpuSamplerRelease(m_scene_sampler);
			m_scene_sampler = nullptr;
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
		m_sampler_anisotropy = 1;
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
		m_max_storage_binding_size = std::min<std::uint64_t>(limits.maxStorageBufferBindingSize, limits.maxBufferSize);
		m_max_compute_workgroups = limits.maxComputeWorkgroupsPerDimension;
		std::ifstream cullInput("/assets/tiled_light_cull.wgsl");
		const std::string cullCode(std::istreambuf_iterator<char>(cullInput), {});
		if (cullCode.empty()) { error = "Missing tiled light cull WGSL"; return false; }
		WGPUShaderSourceWGSL cullSource = WGPU_SHADER_SOURCE_WGSL_INIT;
		cullSource.code = {cullCode.data(), cullCode.size()};
		WGPUShaderModuleDescriptor cullShaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		cullShaderDescriptor.nextInChain = &cullSource.chain;
		const auto cullShader = wgpuDeviceCreateShaderModule(device, &cullShaderDescriptor);
		std::array<WGPUBindGroupLayoutEntry, 3> cullEntries{};
		for (unsigned i = 0; i < cullEntries.size(); ++i)
		{
			cullEntries[i].binding = i;
			cullEntries[i].visibility = WGPUShaderStage_Compute;
			cullEntries[i].buffer.type = i == 0 ? WGPUBufferBindingType_Uniform : (i == 1 ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage);
		}
		WGPUBindGroupLayoutDescriptor cullLayoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		cullLayoutDescriptor.entryCount = cullEntries.size();
		cullLayoutDescriptor.entries = cullEntries.data();
		m_tile_cull_layout = wgpuDeviceCreateBindGroupLayout(device, &cullLayoutDescriptor);
		WGPUPipelineLayoutDescriptor cullPipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		cullPipelineLayoutDescriptor.bindGroupLayoutCount = 1;
		cullPipelineLayoutDescriptor.bindGroupLayouts = &m_tile_cull_layout;
		const auto cullPipelineLayout = wgpuDeviceCreatePipelineLayout(device, &cullPipelineLayoutDescriptor);
		WGPUComputePipelineDescriptor cullPipelineDescriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		cullPipelineDescriptor.layout = cullPipelineLayout;
		cullPipelineDescriptor.compute.module = cullShader;
		cullPipelineDescriptor.compute.entryPoint = {"computeMain", 11};
		m_tile_cull_pipeline = wgpuDeviceCreateComputePipeline(device, &cullPipelineDescriptor);
		wgpuPipelineLayoutRelease(cullPipelineLayout);
		wgpuShaderModuleRelease(cullShader);
		if (!m_tile_cull_pipeline) { error = "Failed to create tiled light cull pipeline"; return false; }
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
		std::array<WGPUVertexAttribute, 8> attributes{};
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
		attributes[7].format = WGPUVertexFormat_Float32x2;
		attributes[7].offset = offsetof(MeshVertex, uv2);
		attributes[7].shaderLocation = 7;
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
		std::array<WGPUBindGroupLayoutEntry, 8> frameEntries{};
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
		frameEntries[6].binding = 6;
		frameEntries[6].visibility = WGPUShaderStage_Fragment;
		frameEntries[6].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		frameEntries[7].binding = 7;
		frameEntries[7].visibility = WGPUShaderStage_Fragment;
		frameEntries[7].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
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
		std::array<WGPUBindGroupLayoutEntry, 5> inputEntries{};
		for (unsigned i = 0; i < 4; ++i)
		{
			inputEntries[i].binding = i;
			inputEntries[i].visibility = WGPUShaderStage_Fragment;
			inputEntries[i].texture.sampleType = i == 0 ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
			inputEntries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		inputEntries[4].binding = 4;
		inputEntries[4].visibility = WGPUShaderStage_Fragment;
		inputEntries[4].sampler.type = WGPUSamplerBindingType_Filtering;
		layoutDescriptor.entryCount = inputEntries.size();
		layoutDescriptor.entries = inputEntries.data();
		m_input_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		std::array<WGPUBindGroupLayout, 3> layouts{m_frame_layout, m_material_layout, m_input_layout};
		WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pipelineLayoutDescriptor.bindGroupLayoutCount = layouts.size();
		pipelineLayoutDescriptor.bindGroupLayouts = layouts.data();
		const auto pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
		descriptor.layout = pipelineLayout;
		for (unsigned target = 0; target < 2; ++target)
		{
			color.format = target ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
			depth.depthCompare = target ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Less;
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
		pipelineLayoutDescriptor.bindGroupLayoutCount = 2;
		const auto shadowLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
		descriptor.layout = shadowLayout;
		depth.depthWriteEnabled = WGPUOptionalBool_True;
		descriptor.primitive.cullMode = WGPUCullMode_None;
		descriptor.primitive.frontFace = WGPUFrontFace_CCW;
		depth.depthCompare = WGPUCompareFunction_Less;
		m_shadow_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);

		fragment.entryPoint = {"auxiliaryMain", 13};
		std::array<WGPUColorTargetState, 3> auxiliaryTargets{};
		for (auto& target: auxiliaryTargets) { target = WGPU_COLOR_TARGET_STATE_INIT; }
		auxiliaryTargets[0].format = WGPUTextureFormat_R32Float;
		auxiliaryTargets[1].format = WGPUTextureFormat_RGBA8Unorm;
		auxiliaryTargets[2].format = WGPUTextureFormat_RGBA8UnormSrgb;
		fragment.targetCount = auxiliaryTargets.size();
		fragment.targets = auxiliaryTargets.data();
		depth.depthCompare = WGPUCompareFunction_LessEqual;
		m_auxiliary_pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
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
		sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
		m_scene_sampler = wgpuDeviceCreateSampler(device, &sampler);
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
		return m_white && m_sampler && m_shadow_pipeline && m_auxiliary_pipeline && std::all_of(m_pipelines.begin(), m_pipelines.end(), [](auto pipeline) { return pipeline != nullptr; });
	}


	bool WebMeshRenderer::UploadShader(WGPUDevice device, std::uint32_t slot, std::string_view wgsl)
	{
		if (slot >= 128 || wgsl.empty() || wgsl.size() > 1024 * 1024 || (slot < m_custom_pipelines.size() && m_custom_pipelines[slot][0]))
		{
			error = "Invalid custom shader slot, source size, or immutable slot already populated";
			return false;
		}
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {wgsl.data(), wgsl.size()};
		WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shaderDescriptor.nextInChain = &source.chain;
		const auto shader = wgpuDeviceCreateShaderModule(device, &shaderDescriptor);
		std::array<WGPUVertexAttribute, 8> attributes{};
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
		attributes[7].format = WGPUVertexFormat_Float32x2;
		attributes[7].offset = offsetof(MeshVertex, uv2);
		attributes[7].shaderLocation = 7;
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
		const std::array<WGPUBindGroupLayout, 3> layouts{m_frame_layout, m_material_layout, m_input_layout};
		WGPUPipelineLayoutDescriptor layoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.bindGroupLayoutCount = layouts.size();
		layoutDescriptor.bindGroupLayouts = layouts.data();
		const auto layout = wgpuDeviceCreatePipelineLayout(device, &layoutDescriptor);
		descriptor.layout = layout;
		std::array<WGPURenderPipeline, 18> pipelines{};
		for (unsigned target = 0; target < 2; ++target)
		{
			color.format = target ? WGPUTextureFormat_RGBA16Float : WGPUTextureFormat_RGBA8Unorm;
			depth.depthCompare = target ? WGPUCompareFunction_LessEqual : WGPUCompareFunction_Less;
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
					pipelines[target * 9 + blend * 3 + winding] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
				}
			}
		}
		const std::array<WGPUBindGroupLayout, 2> auxiliaryLayouts{m_shadow_frame_layout, m_material_layout};
		layoutDescriptor.bindGroupLayoutCount = auxiliaryLayouts.size();
		layoutDescriptor.bindGroupLayouts = auxiliaryLayouts.data();
		const auto auxiliaryLayout = wgpuDeviceCreatePipelineLayout(device, &layoutDescriptor);
		descriptor.layout = auxiliaryLayout;
		descriptor.primitive.cullMode = WGPUCullMode_None;
		depth.depthWriteEnabled = WGPUOptionalBool_True;
		depth.depthCompare = WGPUCompareFunction_LessEqual;
		std::array<WGPUColorTargetState, 3> targets{};
		for (auto& target: targets) { target = WGPU_COLOR_TARGET_STATE_INIT; }
		targets[0].format = WGPUTextureFormat_R32Float;
		targets[1].format = WGPUTextureFormat_RGBA8Unorm;
		targets[2].format = WGPUTextureFormat_RGBA8UnormSrgb;
		fragment.entryPoint = {"auxiliaryMain", 13};
		fragment.targetCount = targets.size();
		fragment.targets = targets.data();
		std::array<WGPURenderPipeline, 3> auxiliary{};
		for (unsigned winding = 0; winding < auxiliary.size(); ++winding)
		{
			descriptor.primitive.cullMode = winding == 0 ? WGPUCullMode_None : WGPUCullMode_Back;
			descriptor.primitive.frontFace = winding == 2 ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
			auxiliary[winding] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
		}
		wgpuPipelineLayoutRelease(auxiliaryLayout);
		wgpuPipelineLayoutRelease(layout);
		wgpuShaderModuleRelease(shader);
		if (!std::all_of(auxiliary.begin(), auxiliary.end(), [](auto pipeline) { return pipeline != nullptr; })
		        || !std::all_of(pipelines.begin(), pipelines.end(), [](auto pipeline) { return pipeline != nullptr; }))
		{
			for (auto pipeline: pipelines) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); } }
			for (auto pipeline: auxiliary) { if (pipeline) { wgpuRenderPipelineRelease(pipeline); } }
			error = "Custom Web material pipeline creation failed";
			return false;
		}
		if (m_custom_pipelines.size() <= slot) { m_custom_pipelines.resize(slot + 1); }
		m_custom_pipelines[slot] = pipelines;
		if (m_custom_auxiliary.size() <= slot) { m_custom_auxiliary.resize(slot + 1); }
		m_custom_auxiliary[slot] = auxiliary;
		return true;
	}

	bool WebMeshRenderer::Upload(WGPUDevice device, WGPUQueue queue, std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices)
	{
		if (!Allocate(device, queue, slot, vertices, indices)) return false;
		WriteSlice(queue, slot, false, 0, vertices.data(), vertices.size_bytes());
		WriteSlice(queue, slot, true, 0, indices.data(), indices.size_bytes());
		return true;
	}

	bool WebMeshRenderer::Allocate(WGPUDevice device, WGPUQueue queue, std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices)
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
			if (!std::isfinite(vertex.tangent.w) || !std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y) || !std::isfinite(vertex.uv2.x) || !std::isfinite(vertex.uv2.y))
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
		mesh.vertices = Buffer(device, queue, WGPUBufferUsage_Vertex, nullptr, vertices.size_bytes());
		mesh.indices = Buffer(device, queue, WGPUBufferUsage_Index, nullptr, indices.size_bytes());
		mesh.indexCount = static_cast<std::uint32_t>(indices.size());
		mesh.jointCount = jointCount;
		mesh.boundsMin = mesh.boundsMax = vertices.front().position;
		for (const auto& vertex: vertices)
		{
			mesh.boundsMin = glm::min(mesh.boundsMin, vertex.position);
			mesh.boundsMax = glm::max(mesh.boundsMax, vertex.position);
		}
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
		mesh.bytes = bytes;
		return true;
	}

	void WebMeshRenderer::WriteSlice(WGPUQueue queue, std::uint32_t slot, bool indices, std::size_t offset, const void* data, std::size_t bytes)
	{
		const auto& mesh = m_meshes[slot];
		wgpuQueueWriteBuffer(queue, indices ? mesh.indices : mesh.vertices, offset, data, bytes);
	}

	void WebMeshRenderer::Release(std::uint32_t slot)
	{
		if (slot >= m_meshes.size()) { return; }
		auto& mesh = m_meshes[slot];
		Destroy(mesh.vertices);
		Destroy(mesh.indices);
		m_bytes -= mesh.bytes;
		mesh = {};
	}

	std::expected<std::vector<gpu::MeshGpuInstance>, std::string> WebMeshRenderer::Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures) const
	{
		const auto plan = PlanShadows(packet, m_max_texture_dimension);
		const std::size_t shadowMaps = plan.regions.size();
		const bool requestedShadows = packet.directionalShadowEnabled || !plan.selected.empty();
		if (requestedShadows && plan.regions.empty())
		{
			return std::unexpected("WebGPU adapter maxTextureDimension2D cannot fit the selected native shadow maps");
		}
		if ((packet.pointLights.size() + packet.spotLights.size()) > m_max_storage_binding_size / 64)
		{
			return std::unexpected("WebGPU adapter storage buffer limit cannot fit local lights");
		}
		const auto finite = [](glm::vec4 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		};
		for (const auto value: {packet.sunDirectionIntensity, packet.sunColor, packet.ambientColor, packet.skyHorizonColor, packet.skyZenithColor, packet.skyVoidColor, packet.skyParams, packet.shadingParams, packet.cameraWorldPos,
		        packet.fogParams, packet.objectAmbient, packet.objectLight0Direction, packet.objectLight0Color, packet.objectLight1Direction, packet.objectLight1Color})
		{
			if (!finite(value))
			{
				return std::unexpected("Nonfinite mesh lighting constants");
			}
		}
		for (unsigned i = 0; i < 4; ++i)
		{
			if (!finite(packet.view[i]) || !finite(packet.proj[i])) { return std::unexpected("Nonfinite mesh camera matrix"); }
		}
		if (!std::isfinite(packet.elapsedTime) || std::abs(glm::determinant(packet.proj * packet.view)) < 1e-12f)
		{
			return std::unexpected("Invalid mesh camera inverse or elapsed time");
		}
		for (const auto blob: packet.blobShadows)
		{
			if (!finite(blob) || std::abs(blob.w) < 1e-6f) { return std::unexpected("Invalid blob shadow bounds"); }
		}
		if (packet.darknessVolumes.size() % 2 != 0) { return std::unexpected("Darkness volumes require centre/fade and half-extents/yaw pairs"); }
		for (const auto volume: packet.darknessVolumes)
		{
			if (!finite(volume)) { return std::unexpected("Nonfinite darkness volume"); }
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
		for (const auto& mesh: packet.render3D.meshes)
		{
			if (mesh.material.grass && !std::isfinite(mesh.material.grassWindStrength))
			{
				return std::unexpected("Invalid grass wind strength");
			}
			if (mesh.proceduralKind > 2 || !finite(mesh.effectTint) || !finite(mesh.effectParams))
			{
				return std::unexpected("Invalid procedural material kind or effect parameters");
			}
			if (mesh.customShaderSlot != ~0u && (mesh.customShaderSlot >= m_custom_pipelines.size() || !m_custom_pipelines[mesh.customShaderSlot][0]))
			{
				return std::unexpected("Custom Web material references an unpopulated shader slot");
			}
		}
		const auto instances = gpu::PlanMeshPass(packet.render3D);
		if (!instances)
		{
			return std::unexpected(instances.error());
		}
		if (packet.renderLightingEnabled)
		{
			constexpr std::uint32_t kSupportedFlags = 1 | 2 | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048 | 4096 | 8192 | 16384 | 32768 | gpu::kMeshMirrored;
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

	void WebMeshRenderer::PrepareFrame(WGPUDevice device, WGPUQueue queue, unsigned width, unsigned height,
	        const RenderFramePacket& packet, std::span<const gpu::MeshGpuInstance> instances,
	        std::span<const WGPUTexture> textures, std::unique_ptr<WebMeshFrame>& frame, bool hdr)
	{
		const std::uint64_t tilesX = (std::uint64_t(width) + 15) / 16;
		const std::uint64_t tilesY = (std::uint64_t(height) + 15) / 16;
		const std::uint64_t stride = std::min<std::uint64_t>(packet.pointLights.size() + packet.spotLights.size(), 128) + 1;
		if (!tilesX || !tilesY || tilesX > m_max_compute_workgroups || tilesY > m_max_compute_workgroups ||
		    tilesX * tilesY > UINT32_MAX / stride || tilesX * tilesY > m_max_storage_binding_size / (stride * sizeof(std::uint32_t)))
		{
			error = "WEBGPU_LIGHT_TILES: viewport exceeds tile buffer or dispatch limits";
			return;
		}
		const std::uint64_t tileBytes = tilesX * tilesY * stride * sizeof(std::uint32_t);
		const int anisotropy = ClampTextureAnisotropy(packet.anisotropy);
		if (anisotropy != m_sampler_anisotropy)
		{
			WGPUSamplerDescriptor sampler = WGPU_SAMPLER_DESCRIPTOR_INIT;
			sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_Repeat;
			sampler.magFilter = sampler.minFilter = WGPUFilterMode_Linear;
			sampler.mipmapFilter = WGPUMipmapFilterMode_Linear;
			sampler.lodMinClamp = 0;
			sampler.lodMaxClamp = 32;
			sampler.maxAnisotropy = static_cast<std::uint16_t>(anisotropy);
			const auto replacement = wgpuDeviceCreateSampler(device, &sampler);
			if (!replacement)
			{
				error = "WEBGPU_SAMPLER: failed to create mesh sampler";
				return;
			}
			// Existing frame bind groups retain their sampler through GPU completion.
			wgpuSamplerRelease(m_sampler);
			m_sampler = replacement;
			m_sampler_anisotropy = anisotropy;
		}
		const auto clip = glm::transpose(packet.proj * packet.view);
		frame = std::make_unique<WebMeshFrame>();
		frame->prepared = std::make_unique<WebMeshPreparedData>();
		frame->tilesX = static_cast<unsigned>(tilesX);
		frame->tilesY = static_cast<unsigned>(tilesY);
		// Use the same zero-to-one clip planes as the native FrameConstants.
		// Animated and custom/procedural vertices can leave their authored bounds.
		const std::array<glm::vec4, 6> planes{clip[3] + clip[0], clip[3] - clip[0], clip[3] + clip[1], clip[3] - clip[1], clip[2], clip[3] - clip[2]};
		auto& visible = frame->prepared->cameraVisible;
		visible.reserve(packet.render3D.meshes.size());
		auto& distanceVisible = frame->prepared->distanceVisible;
		distanceVisible.reserve(packet.render3D.meshes.size());
		for (const auto& instance: packet.render3D.meshes)
		{
			if (!instance.skinMatrices.empty() || instance.customShaderSlot != UINT32_MAX || instance.proceduralKind != 0 || instance.material.sky)
			{
				visible.push_back(true);
				++frame->cameraVisibleCount;
				distanceVisible.push_back(true);
				continue;
			}
			const auto& mesh = m_meshes[instance.mesh];
			const auto center = glm::vec3(instance.world * glm::vec4((mesh.boundsMin + mesh.boundsMax) * 0.5f, 1.0f));
			const auto half = (mesh.boundsMax - mesh.boundsMin) * 0.5f;
			const auto inflation = instance.material.grass ? 1.25f * (mesh.boundsMax.y - mesh.boundsMin.y) * glm::length(glm::vec3(instance.world[1])) + 0.05f : 0.1f;
			const auto extent = glm::abs(glm::vec3(instance.world[0])) * half.x + glm::abs(glm::vec3(instance.world[1])) * half.y + glm::abs(glm::vec3(instance.world[2])) * half.z + glm::vec3(inflation);
			const auto fade = instance.material.distanceFade;
			const auto delta = glm::max(glm::abs(center - glm::vec3(packet.cameraWorldPos)) - extent, glm::vec3(0.0f));
			const bool inRange = fade.x < 0.0f || fade.y <= fade.x || glm::dot(delta, delta) <= fade.y * fade.y;
			distanceVisible.push_back(inRange);
			bool inside = true;
			for (const auto& plane: planes)
			{
				if (glm::dot(glm::vec3(plane), center) + plane.w + glm::dot(glm::abs(glm::vec3(plane)), extent) < 0.0f)
				{
					inside = false;
					break;
				}
			}
			if (!inRange) { ++frame->distanceCulledCount; }
			else if (!inside) { ++frame->frustumCulledCount; }
			else { ++frame->cameraVisibleCount; }
			visible.push_back(inside && inRange);
		}

		using FrameConstants = WebMeshPreparedData::FrameConstants;
		using ShadowData = WebMeshPreparedData::ShadowData;
		std::vector<ShadowData> shadows;
		glm::vec4 splits(0);
		const auto plan = PlanShadows(packet, m_max_texture_dimension);
		const auto requested = std::count_if(packet.pointLights.begin(), packet.pointLights.end(), [](const auto& light) { return light.castsShadow; })
		        + std::count_if(packet.spotLights.begin(), packet.spotLights.end(), [](const auto& light) { return light.castsShadow; });
		const auto dropped = hdr ? std::size_t(requested) - plan.selected.size() : 0;
		if (dropped != m_dropped_shadow_casters)
		{
			m_dropped_shadow_casters = dropped;
			if (dropped) { std::fprintf(stderr, "WebGPU local shadow budget: %zu shadow-casting lights are unshadowed (48 map entries).\n", dropped); }
		}
		auto appendShadow = [&](glm::mat4 matrix, glm::vec4 params, glm::vec4 lightPosRange = glm::vec4(0), unsigned = 512)
		{
			shadows.push_back({matrix[0], matrix[1], matrix[2], matrix[3], params, lightPosRange, plan.regions[shadows.size()]});
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
		std::vector<float> shadowIndices(packet.pointLights.size() + packet.spotLights.size(), -1.0f);
		if (hdr) for (const auto& candidate: plan.selected)
		{
			const auto index = candidate.lightIndex + (candidate.lightType ? packet.pointLights.size() : 0);
			shadowIndices[index] = float(shadows.size());
			if (!candidate.lightType)
			{
				const auto& light = packet.pointLights[candidate.lightIndex];
				for (int face = 0; face < 6; ++face)
				{
					appendShadow(glm::perspective(2.0f * std::atan(396.0f / 384.0f), 1.0f, 0.1f, light.radius) * glm::lookAt(light.position, light.position + directions[face], ups[face]),
					        glm::vec4(0, 2.0f * (396.0f / 384.0f) / 384.0f, light.sourceRadius, 1), glm::vec4(light.position, light.radius), 384);
				}
			}
			else
			{
				const auto& light = packet.spotLights[candidate.lightIndex];
				const auto direction = glm::normalize(light.direction);
				const auto up = std::abs(direction.y) > 0.95f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
				appendShadow(glm::perspective(std::min(2 * (light.outerAngleRad + glm::radians(4.0f)), glm::radians(175.0f)), 1.0f, 0.1f, light.radius) * glm::lookAt(light.position, light.position + direction, up),
				        glm::vec4(0, 2.0f * std::tan(std::min(light.outerAngleRad + glm::radians(4.0f), glm::radians(87.5f))) / 512, light.sourceRadius, 1), glm::vec4(light.position, light.radius));
			}
		}
		for (std::size_t i = 0; i < packet.pointLights.size(); ++i)
		{
			const auto& light = packet.pointLights[i];
			lights.push_back({glm::vec4(light.position, light.radius), glm::vec4(light.color, light.intensity), glm::vec4(0, -1, 0, 0), glm::vec4(0, 0, shadowIndices[i], 0)});
		}
		for (std::size_t i = 0; i < packet.spotLights.size(); ++i)
		{
			const auto& light = packet.spotLights[i];
			const auto direction = glm::normalize(light.direction);
			lights.push_back({glm::vec4(light.position, light.radius), glm::vec4(light.color, light.intensity), glm::vec4(direction, 1), glm::vec4(std::cos(light.innerAngleRad), std::cos(light.outerAngleRad), shadowIndices[packet.pointLights.size() + i], 0)});
		}
		const auto viewRows = glm::transpose(packet.view);
		FrameConstants constants{clip[0],
		        clip[1],
		        clip[2],
		        clip[3],
		        glm::vec4(packet.elapsedTime, hdr ? 1 : 0, ClampTextureLodBias(packet.mipLodBias), static_cast<float>(width)),
		        packet.cameraWorldPos,
		        packet.sunDirectionIntensity,
		        packet.sunColor,
		        packet.ambientColor,
		        packet.skyHorizonColor,
		        packet.skyZenithColor,
		        packet.skyVoidColor,
		        glm::vec4(packet.skyParams.x, packet.skyParams.y, packet.postEffects.volumetrics ? packet.skyParams.z : 0.0f, packet.skyParams.w),
		        packet.shadingParams,
		        glm::uvec4(lights.size(), hdr && packet.directionalShadowEnabled ? 1 : 0, 0, packet.contactShadows ? 1 : 0),
		        -viewRows[2],
		        splits, packet.fogParams, packet.objectAmbient, packet.objectLight0Direction, packet.objectLight0Color, packet.objectLight1Direction, packet.objectLight1Color};
		const auto inverseClip = glm::transpose(glm::inverse(packet.proj * packet.view));
		constants.invClip0 = inverseClip[0]; constants.invClip1 = inverseClip[1];
		constants.invClip2 = inverseClip[2]; constants.invClip3 = inverseClip[3];
		constants.volumeCounts = glm::uvec4(std::min(packet.blobShadows.size(), std::size_t(64)), std::min(packet.blobCharacterCount, 64u), std::min(packet.darknessVolumes.size() / 2, std::size_t(16)), 0);
		constants.visibilityCamera = packet.cameraWorldPos;
		constants.grassBenders = packet.grassBenders.positions;
		constants.grassBenderParams = packet.grassBenders.parameters;
		std::copy_n(packet.blobShadows.begin(), constants.volumeCounts.x, constants.blobs.begin());
		std::copy_n(packet.darknessVolumes.begin(), constants.volumeCounts.z * 2, constants.darkness.begin());
		const auto shadowCount = shadows.size();
		frame->prepared->constants = constants;
		frame->prepared->shadowCount = shadowCount;
		if (shadows.empty())
		{
			shadows.emplace_back();
		}
		frame->prepared->shadows = shadows;
		frame->shadows = Buffer(device, queue, WGPUBufferUsage_Storage, shadows.data(), shadows.size() * sizeof(ShadowData));
		// Queue submissions execute in order, so frames can reuse the atlas while
		// retaining its ownership until their own completion callback retires.
		auto& cache = shadowCount ? m_shadow_targets : m_shadow_fallback;
		if (cache && (cache->width != (shadowCount ? plan.width : 1u) || cache->height != (shadowCount ? plan.height : 1u))) { cache.reset(); }
		if (!cache)
		{
			cache = std::make_shared<WebShadowTargets>();
			cache->width = shadowCount ? plan.width : 1u;
			cache->height = shadowCount ? plan.height : 1u;
			WGPUTextureDescriptor texture = WGPU_TEXTURE_DESCRIPTOR_INIT;
			texture.size = WGPUExtent3D{cache->width, cache->height, 1};
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
			view.dimension = WGPUTextureViewDimension_2D;
			cache->sceneSample = wgpuTextureCreateView(cache->color, &view);
		}
		frame->shadowTargets = cache;
		frame->directional.atlas = cache->sceneSample;
		frame->directional.enabled = hdr && packet.directionalShadowEnabled;
		frame->directional.splits = splits;
		if (frame->directional.enabled)
		{
			for (int i = 0; i < 3; ++i)
			{
				const auto& shadow = shadows[i];
				frame->directional.viewProj[i] = glm::mat4(shadow.c0, shadow.c1, shadow.c2, shadow.c3);
				frame->directional.regions[i] = shadow.region;
				frame->directional.depthBias[i] = shadow.params.x;
				frame->directional.normalOffset[i] = shadow.params.y;
				frame->directional.penumbraScale[i] = shadow.params.z;
			}
		}
		if (lights.empty())
		{
			lights.emplace_back();
		}
		frame->lights = Buffer(device, queue, WGPUBufferUsage_Storage, lights.data(), lights.size() * sizeof(LightData));
		frame->uniform = Buffer(device, queue, WGPUBufferUsage_Uniform, &constants, sizeof(constants));
		struct CullData { glm::vec4 view0, view1, view2, projection; glm::uvec4 dimensions; };
		const CullData cull{viewRows[0], viewRows[1], viewRows[2],
		        glm::vec4(packet.cameraNearPlane, 1.0f / packet.proj[0][0], 1.0f / packet.proj[1][1], std::abs(packet.proj[3][3] - 1.0f) < 1e-6f ? 1.0f : 0.0f),
		        glm::uvec4(width, height, frame->tilesX, constants.counts.x)};
		frame->tileCullUniform = Buffer(device, queue, WGPUBufferUsage_Uniform, &cull, sizeof(cull));
		WGPUBufferDescriptor tileDescriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
		tileDescriptor.size = tileBytes;
		tileDescriptor.usage = WGPUBufferUsage_Storage;
		frame->lightTiles = wgpuDeviceCreateBuffer(device, &tileDescriptor);
		std::array<WGPUBindGroupEntry, 3> cullBindings{};
		cullBindings[0].binding = 0; cullBindings[0].buffer = frame->tileCullUniform; cullBindings[0].size = sizeof(cull);
		cullBindings[1].binding = 1; cullBindings[1].buffer = frame->lights; cullBindings[1].size = wgpuBufferGetSize(frame->lights);
		cullBindings[2].binding = 2; cullBindings[2].buffer = frame->lightTiles; cullBindings[2].size = tileBytes;
		WGPUBindGroupDescriptor cullGroup = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		cullGroup.layout = m_tile_cull_layout; cullGroup.entryCount = cullBindings.size(); cullGroup.entries = cullBindings.data();
		frame->tileCullBindings = wgpuDeviceCreateBindGroup(device, &cullGroup);
		struct WebMeshInstance
		{
			gpu::MeshGpuInstance mesh;
			glm::vec4 distanceFade;
		};
		static_assert(sizeof(WebMeshInstance) == 208);
		std::vector<WebMeshInstance> materialInstances(instances.size());
		for (std::size_t i = 0; i < materialInstances.size(); ++i)
		{
			materialInstances[i].mesh = instances[i];
			const auto& material = packet.render3D.meshes[i].material;
			materialInstances[i].distanceFade = glm::vec4(material.distanceFade, material.grassWindStrength, 0.0f);
			std::uint32_t textureFlags = 0;
			for (unsigned map = 0; map < 5; ++map)
			{
				if (packet.render3D.meshes[i].textures[map] != UINT32_MAX) { textureFlags |= 1u << map; }
			}
			materialInstances[i].mesh.factors.w = static_cast<float>(textureFlags);
			if (packet.render3D.meshes[i].material.grass)
			{
				materialInstances[i].mesh.metadata.w |= 32768;
			}
		}
		const auto originalCount = materialInstances.size();
		materialInstances.resize(originalCount * 2);
		for (std::size_t i = 0; i < originalCount; ++i)
		{
			materialInstances[originalCount + i] = materialInstances[i];
			materialInstances[originalCount + i].mesh.metadata.w |= 1u << 30;
		}
		frame->instances = Buffer(device, queue, WGPUBufferUsage_Storage, materialInstances.data(), materialInstances.size() * sizeof(WebMeshInstance));
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
		struct EffectData { glm::vec4 tint, params; };
		std::vector<EffectData> effects;
		for (const auto& mesh: packet.render3D.meshes)
		{
			effects.push_back({mesh.effectTint, glm::vec4(glm::vec3(mesh.effectParams), static_cast<float>(mesh.proceduralKind))});
		}
		frame->effects = Buffer(device, queue, WGPUBufferUsage_Storage, effects.data(), effects.size() * sizeof(EffectData));


		std::array<WGPUBindGroupEntry, 8> entries{};
		entries[0].binding = 0;
		entries[0].buffer = frame->uniform;
		entries[0].size = sizeof(constants);
		entries[1].binding = 1;
		entries[1].buffer = frame->instances;
		entries[1].size = wgpuBufferGetSize(frame->instances);
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
		entries[6].binding = 6;
		entries[6].buffer = frame->effects;
		entries[6].size = effects.size() * sizeof(EffectData);
		entries[7].binding = 7;
		entries[7].buffer = frame->lightTiles;
		entries[7].size = tileBytes;
		WGPUBindGroupDescriptor bindings = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindings.layout = m_frame_layout;
		bindings.entries = entries.data();
		bindings.entryCount = entries.size();
		frame->bindings = wgpuDeviceCreateBindGroup(device, &bindings);
		for (std::size_t meshIndex = 0; meshIndex < packet.render3D.meshes.size(); ++meshIndex)
		{
			const auto& instance = packet.render3D.meshes[meshIndex];
			const bool shadowDraw = shadowCount && instance.castShadows && !instance.material.alphaBlend && !instance.material.sky && frame->prepared->distanceVisible[meshIndex];
			if (!frame->prepared->cameraVisible[meshIndex] && !shadowDraw)
			{
				frame->materials.push_back(nullptr);
				continue;
			}
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
		bindings.layout = m_shadow_frame_layout;
		bindings.entryCount = 3;
		frame->auxiliaryBindings = wgpuDeviceCreateBindGroup(device, &bindings);
	}


	void WebMeshRenderer::EncodeAuxiliary(WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder,
	        unsigned width, unsigned height, const RenderFramePacket& packet,
	        std::span<const gpu::MeshGpuInstance> instances, std::span<const WGPUTexture> textures,
	        const WebOpaqueInputs& inputs, std::unique_ptr<WebMeshFrame>& frame)
	{
		if (!frame)
		{
			PrepareFrame(device, queue, width, height, packet, instances, textures, frame, true);
		}
		if (!frame) { return; }
		frame->opaqueDepthAttachment = inputs.depthAttachment;
		std::array<WGPURenderPassColorAttachment, 3> attachments{};
		const std::array<WGPUTextureView, 3> views{inputs.sceneDepth, inputs.normalRoughness, inputs.albedo};
		for (unsigned i = 0; i < attachments.size(); ++i)
		{
			attachments[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			attachments[i].view = views[i];
			attachments[i].loadOp = WGPULoadOp_Clear;
			attachments[i].storeOp = WGPUStoreOp_Store;
		}
		attachments[0].clearValue = {1, 1, 1, 1};
		attachments[1].clearValue = {0.5, 0.5, 1, 0};
		WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		depth.view = inputs.depthAttachment;
		depth.depthLoadOp = WGPULoadOp_Clear;
		depth.depthStoreOp = WGPUStoreOp_Store;
		depth.depthClearValue = 1;
		WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		descriptor.colorAttachmentCount = attachments.size();
		descriptor.colorAttachments = attachments.data();
		descriptor.depthStencilAttachment = &depth;
		const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
		wgpuRenderPassEncoderSetPipeline(pass, m_auxiliary_pipeline);
		wgpuRenderPassEncoderSetBindGroup(pass, 0, frame->auxiliaryBindings, 0, nullptr);
		for (unsigned i = 0; i < packet.render3D.meshes.size(); ++i)
		{
			const auto& instance = packet.render3D.meshes[i];
			if (instance.material.alphaBlend || !frame->prepared->cameraVisible[i]) { continue; }
			const unsigned winding = instance.material.doubleSided ? 0 : ((instances[i].metadata.w & gpu::kMeshMirrored) ? 2 : 1);
			wgpuRenderPassEncoderSetPipeline(pass, instance.customShaderSlot != ~0u ? m_custom_auxiliary[instance.customShaderSlot][winding] : m_auxiliary_pipeline);
			const auto& mesh = m_meshes[instance.mesh];
			wgpuRenderPassEncoderSetBindGroup(pass, 1, frame->materials[i], 0, nullptr);
			wgpuRenderPassEncoderSetVertexBuffer(pass, 0, mesh.vertices, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderSetIndexBuffer(pass, mesh.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderDrawIndexed(pass, mesh.indexCount, 1, 0, 0, i);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
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
	        bool hdr,
	        const WebMeshInputs& inputs)
	{
		if (!frame)
		{
			PrepareFrame(device, queue, width, height, packet, instances, textures, frame, hdr);
		}
		if (!frame) { return; }
		if (!frame->tilesEncoded)
		{
			WGPUComputePassDescriptor descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
			const auto pass = wgpuCommandEncoderBeginComputePass(encoder, &descriptor);
			wgpuComputePassEncoderSetPipeline(pass, m_tile_cull_pipeline);
			wgpuComputePassEncoderSetBindGroup(pass, 0, frame->tileCullBindings, 0, nullptr);
			wgpuComputePassEncoderDispatchWorkgroups(pass, frame->tilesX, frame->tilesY, 1);
			wgpuComputePassEncoderEnd(pass);
			wgpuComputePassEncoderRelease(pass);
			frame->tilesEncoded = true;
		}
		if (!frame->inputBindings)
		{
			std::array<WGPUBindGroupEntry, 5> inputEntries{};
			const auto fallback = wgpuTextureCreateView(m_white, nullptr);
			const std::array<WGPUTextureView, 4> views{inputs.sceneDepth, inputs.normalRoughness, inputs.albedo, inputs.ambientOcclusion};
			for (unsigned i = 0; i < 4; ++i)
			{
				inputEntries[i].binding = i;
				inputEntries[i].textureView = views[i] ? views[i] : fallback;
			}
			inputEntries[4].binding = 4;
			inputEntries[4].sampler = m_scene_sampler;
			WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			descriptor.layout = m_input_layout;
			descriptor.entryCount = inputEntries.size();
			descriptor.entries = inputEntries.data();
			frame->inputBindings = wgpuDeviceCreateBindGroup(device, &descriptor);
			wgpuTextureViewRelease(fallback);
		}
		using FrameConstants = WebMeshPreparedData::FrameConstants;
		const auto& constants = frame->prepared->constants;
		const auto& shadows = frame->prepared->shadows;
		const auto shadowCount = frame->prepared->shadowCount;
		std::array<WGPUBindGroupEntry, 3> entries{};
		entries[0].binding = 0;
		entries[0].buffer = frame->uniform;
		entries[0].size = sizeof(FrameConstants);
		entries[1].binding = 1;
		entries[1].buffer = frame->instances;
		entries[1].size = wgpuBufferGetSize(frame->instances);
		entries[2].binding = 2;
		entries[2].buffer = frame->skin;
		entries[2].size = wgpuBufferGetSize(frame->skin);
		WGPUBindGroupDescriptor bindings = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		bindings.entries = entries.data();
		bindings.entryCount = 3;
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
				if (!instance.castShadows || instance.material.alphaBlend || instance.material.sky || !frame->prepared->distanceVisible[i])
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
		if (!frame->opaqueDepthAttachment && !frame->depth)
		{
		WGPUTextureDescriptor depth = WGPU_TEXTURE_DESCRIPTOR_INIT;
		depth.size = {width, height, 1};
		depth.dimension = WGPUTextureDimension_2D;
		depth.format = WGPUTextureFormat_Depth32Float;
		depth.usage = WGPUTextureUsage_RenderAttachment;
		frame->depth = wgpuDeviceCreateTexture(device, &depth);
		frame->depthView = wgpuTextureCreateView(frame->depth, nullptr);
		}
		WGPURenderPassDepthStencilAttachment depthAttachment = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
		depthAttachment.view = frame->opaqueDepthAttachment ? frame->opaqueDepthAttachment : frame->depthView;
		depthAttachment.depthLoadOp = frame->opaqueDepthAttachment ? WGPULoadOp_Load : WGPULoadOp_Clear;
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
		auto drawOrder = gpu::MeshDrawOrder(packet.render3D, packet.view);
		if (hdr)
		{
			const auto opaqueEnd = std::find_if(drawOrder.begin(), drawOrder.end(), [&](auto index) { return packet.render3D.meshes[index].material.alphaBlend; });
			std::vector<std::uint32_t> edges;
			for (unsigned i = 0; i < instances.size(); ++i)
			{
				if ((instances[i].metadata.w & (32 | 64 | 512 | 4)) == (32 | 64 | 4)) { edges.push_back(i + instances.size()); }
			}
			drawOrder.insert(opaqueEnd, edges.begin(), edges.end());
		}
		for (const auto drawIndex: drawOrder)
		{
			const auto index = drawIndex % instances.size();
			const bool edge = drawIndex >= instances.size();
			if (!frame->prepared->cameraVisible[index]) { continue; }
			const auto& instance = packet.render3D.meshes[index];
			const auto& mesh = m_meshes[instance.mesh];
			const auto flags = instances[index].metadata.w;
			const unsigned blend = edge ? 1 : (instance.material.alphaBlend ? (instance.material.additiveBlend ? 2 : 1) : 0);
			const unsigned winding = instance.material.doubleSided ? 0 : ((flags & gpu::kMeshMirrored) ? 2 : 1);
			const auto& pipelines = instance.customShaderSlot != ~0u ? m_custom_pipelines[instance.customShaderSlot] : m_pipelines;
			wgpuRenderPassEncoderSetPipeline(pass, pipelines[(hdr ? 9 : 0) + blend * 3 + winding]);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, frame->bindings, 0, nullptr);
			wgpuRenderPassEncoderSetBindGroup(pass, 2, frame->inputBindings, 0, nullptr);
			wgpuRenderPassEncoderSetBindGroup(pass, 1, frame->materials[index], 0, nullptr);
			wgpuRenderPassEncoderSetVertexBuffer(pass, 0, mesh.vertices, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderSetIndexBuffer(pass, mesh.indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
			wgpuRenderPassEncoderDrawIndexed(pass, mesh.indexCount, 1, 0, 0, drawIndex);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}
} // namespace aether::webgpu
