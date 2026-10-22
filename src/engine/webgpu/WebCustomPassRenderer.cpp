#include "WebCustomPassRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <glm/glm.hpp>

namespace aether::webgpu
{
	namespace
	{
		constexpr std::size_t kAlignment = 256;

		// Mirrors CustomPassWebFrame in shaders/include/CustomPass.slangh (224 bytes).
		struct CustomPassUniform
		{
			glm::vec4 viewProj[4];
			glm::vec4 invViewProj[4];
			glm::vec4 cameraWorldPos;
			glm::vec4 params;
			glm::vec4 color0;
			glm::vec4 color1;
			glm::vec4 viewportTime;
			std::uint32_t dataInfo[4];
		};
		static_assert(sizeof(CustomPassUniform) == 224, "CustomPassUniform must match CustomPassWebFrame");

		bool Finite(const glm::vec4& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		std::size_t DataSlice(std::size_t count)
		{
			// Storage bind offsets must be 256-aligned, so every slice is padded to 16 float4.
			const std::size_t vec4s = std::max<std::size_t>(count, 1);
			return (vec4s * sizeof(glm::vec4) + kAlignment - 1) / kAlignment * kAlignment;
		}

		WGPUBuffer CreateBuffer(WGPUDevice device, WGPUQueue queue, WGPUBufferUsage usage, const void* data, std::size_t size)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = size;
			descriptor.usage = usage | WGPUBufferUsage_CopyDst;
			const auto buffer = wgpuDeviceCreateBuffer(device, &descriptor);
			wgpuQueueWriteBuffer(queue, buffer, 0, data, size);
			return buffer;
		}
	} // namespace

	WebCustomPassFrame::~WebCustomPassFrame()
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

	WebCustomPassRenderer::~WebCustomPassRenderer()
	{
		Shutdown();
	}

	void WebCustomPassRenderer::Shutdown()
	{
		for (auto& [stem, pipelines]: m_pipelines)
		{
			for (auto& pipeline: pipelines)
			{
				if (pipeline)
				{
					wgpuRenderPipelineRelease(pipeline);
					pipeline = nullptr;
				}
			}
		}
		m_pipelines.clear();
		if (m_pipeline_layout)
		{
			wgpuPipelineLayoutRelease(m_pipeline_layout);
			m_pipeline_layout = nullptr;
		}
		if (m_layout)
		{
			wgpuBindGroupLayoutRelease(m_layout);
			m_layout = nullptr;
		}
	}

	bool WebCustomPassRenderer::Initialize(WGPUDevice device)
	{
		std::array<WGPUBindGroupLayoutEntry, 2> entries{};
		entries[0].binding = 0;
		entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[0].buffer.type = WGPUBufferBindingType_Uniform;
		entries[0].buffer.minBindingSize = sizeof(CustomPassUniform);
		entries[1].binding = 1;
		entries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		entries[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		WGPUBindGroupLayoutDescriptor groupDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		groupDescriptor.entryCount = entries.size();
		groupDescriptor.entries = entries.data();
		m_layout = wgpuDeviceCreateBindGroupLayout(device, &groupDescriptor);
		WGPUPipelineLayoutDescriptor layoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.bindGroupLayoutCount = 1;
		layoutDescriptor.bindGroupLayouts = &m_layout;
		m_pipeline_layout = wgpuDeviceCreatePipelineLayout(device, &layoutDescriptor);
		if (!m_layout || !m_pipeline_layout)
		{
			error = "Custom pass bind group layout creation failed";
			return false;
		}
		return true;
	}

	bool WebCustomPassRenderer::HasShader(std::string_view stem) const
	{
		return m_pipelines.find(stem) != m_pipelines.end();
	}

	bool WebCustomPassRenderer::UploadShader(WGPUDevice device, std::string_view stem, std::string_view wgsl)
	{
		if (!m_layout || !m_pipeline_layout)
		{
			error = "WEB_CUSTOM_PASS_SHADER: custom pass renderer is not initialised";
			return false;
		}
		if (stem.empty() || wgsl.empty() || wgsl.size() > kMaxShaderBytes)
		{
			error = "WEB_CUSTOM_PASS_SHADER: invalid custom pass shader stem or source size";
			return false;
		}
		if (HasShader(stem))
		{
			return true;
		}
		if (m_pipelines.size() >= kMaxShaders)
		{
			error = std::format("WEB_CUSTOM_PASS_SHADER: custom pass shader cap of {} reached", kMaxShaders);
			return false;
		}
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {wgsl.data(), wgsl.size()};
		WGPUShaderModuleDescriptor shaderDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shaderDescriptor.nextInChain = &source.chain;
		const auto shader = wgpuDeviceCreateShaderModule(device, &shaderDescriptor);
		if (!shader)
		{
			error = std::format("WEB_CUSTOM_PASS_SHADER: shader module creation failed for '{}'", stem);
			return false;
		}
		// Matches gpu::BlendMode::Alpha in vulkan/GraphicsPipelineFactory.cpp.
		WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
		blend.color.operation = WGPUBlendOperation_Add;
		blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
		blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		blend.alpha.operation = WGPUBlendOperation_Add;
		blend.alpha.srcFactor = WGPUBlendFactor_One;
		blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
		WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
		target.blend = &blend;
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
		fragment.module = shader;
		fragment.entryPoint = {"fragmentMain", 12};
		fragment.targetCount = 1;
		fragment.targets = &target;
		WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
		descriptor.layout = m_pipeline_layout;
		descriptor.vertex.module = shader;
		descriptor.vertex.entryPoint = {"vertexMain", 10};
		descriptor.fragment = &fragment;
		descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
		descriptor.primitive.cullMode = WGPUCullMode_None;
		std::array<WGPURenderPipeline, 2> pipelines{};
		for (unsigned display = 0; display < 2; ++display)
		{
			target.format = display ? WGPUTextureFormat_RGBA8Unorm : WGPUTextureFormat_RGBA16Float;
			pipelines[display] = wgpuDeviceCreateRenderPipeline(device, &descriptor);
		}
		wgpuShaderModuleRelease(shader);
		if (!pipelines[0] || !pipelines[1])
		{
			for (auto pipeline: pipelines)
			{
				if (pipeline)
				{
					wgpuRenderPipelineRelease(pipeline);
				}
			}
			error = std::format("WEB_CUSTOM_PASS_SHADER: pipeline creation failed for '{}'", stem);
			return false;
		}
		m_pipelines.emplace(std::string(stem), pipelines);
		return true;
	}

	std::expected<void, std::string> WebCustomPassRenderer::Prepare(const RenderFramePacket& packet) const
	{
		const auto& passes = packet.renderCustom.passes;
		if (passes.size() > kMaxSubmissions)
		{
			return std::unexpected(std::format("WEB_CUSTOM_PASS_BUDGET: {} custom pass submissions exceed the {} per frame limit", passes.size(), kMaxSubmissions));
		}
		std::size_t total = 0;
		for (std::size_t index = 0; index < passes.size(); ++index)
		{
			const auto& draw = passes[index];
			if (static_cast<unsigned>(draw.stage) > 2)
			{
				return std::unexpected(std::format("WEB_CUSTOM_PASS_STAGE: submission {} has invalid stage {}", index, static_cast<unsigned>(draw.stage)));
			}
			if (!HasShader(draw.shader))
			{
				return std::unexpected(std::format("WEB_CUSTOM_PASS_SHADER: shader '{}' was not uploaded", draw.shader));
			}
			if (draw.data.size() > kMaxVec4PerSubmission)
			{
				return std::unexpected(std::format("WEB_CUSTOM_PASS_BUDGET: submission {} has {} float4 elements, limit is {}", index, draw.data.size(), kMaxVec4PerSubmission));
			}
			total += draw.data.size();
			if (total > kMaxVec4PerFrame)
			{
				return std::unexpected(std::format("WEB_CUSTOM_PASS_BUDGET: custom pass data exceeds {} float4 elements per frame", kMaxVec4PerFrame));
			}
			if (!Finite(draw.params) || !Finite(draw.color0) || !Finite(draw.color1))
			{
				return std::unexpected(std::format("WEB_CUSTOM_PASS_NONFINITE: submission {} has non-finite params or colors", index));
			}
			for (const auto& element: draw.data)
			{
				if (!Finite(element))
				{
					return std::unexpected(std::format("WEB_CUSTOM_PASS_NONFINITE: submission {} has non-finite data", index));
				}
			}
		}
		return {};
	}

	void WebCustomPassRenderer::Encode(WGPUDevice device,
	        WGPUQueue queue,
	        WGPUCommandEncoder encoder,
	        WGPUTextureView color,
	        unsigned width,
	        unsigned height,
	        const RenderFramePacket& packet,
	        CustomPassStage stage,
	        bool hdrTarget,
	        bool clearFirst,
	        std::unique_ptr<WebCustomPassFrame>& frame)
	{
		std::vector<const CustomPassDraw*> draws;
		std::size_t dataBytes = 0;
		for (const auto& draw: packet.renderCustom.passes)
		{
			if (draw.stage == stage)
			{
				draws.push_back(&draw);
				dataBytes += DataSlice(draw.data.size());
			}
		}
		if (draws.empty())
		{
			return;
		}
		if (!frame)
		{
			frame = std::make_unique<WebCustomPassFrame>();
		}
		const auto viewProj = packet.proj * packet.view;
		const auto transposedViewProj = glm::transpose(viewProj);
		const auto transposedInverse = glm::transpose(glm::inverse(viewProj));

		std::vector<std::uint8_t> uniforms(draws.size() * kAlignment, 0);
		std::vector<std::uint8_t> data(dataBytes, 0);
		std::size_t dataOffset = 0;
		std::vector<std::size_t> dataOffsets;
		dataOffsets.reserve(draws.size());
		for (std::size_t i = 0; i < draws.size(); ++i)
		{
			const auto& draw = *draws[i];
			CustomPassUniform record{};
			for (int row = 0; row < 4; ++row)
			{
				record.viewProj[row] = transposedViewProj[row];
				record.invViewProj[row] = transposedInverse[row];
			}
			record.cameraWorldPos = packet.cameraWorldPos;
			record.params = draw.params;
			record.color0 = draw.color0;
			record.color1 = draw.color1;
			record.viewportTime = glm::vec4(static_cast<float>(width), static_cast<float>(height), packet.elapsedTime, 0.0f);
			record.dataInfo[0] = static_cast<std::uint32_t>(draw.data.size());
			std::memcpy(uniforms.data() + i * kAlignment, &record, sizeof(record));
			if (!draw.data.empty())
			{
				std::memcpy(data.data() + dataOffset, draw.data.data(), draw.data.size() * sizeof(glm::vec4));
			}
			dataOffsets.push_back(dataOffset);
			dataOffset += DataSlice(draw.data.size());
		}
		const auto uniformBuffer = CreateBuffer(device, queue, WGPUBufferUsage_Uniform, uniforms.data(), uniforms.size());
		const auto dataBuffer = CreateBuffer(device, queue, WGPUBufferUsage_Storage, data.data(), data.size());
		frame->buffers.push_back(uniformBuffer);
		frame->buffers.push_back(dataBuffer);

		WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		attachment.view = color;
		attachment.loadOp = clearFirst ? WGPULoadOp_Clear : WGPULoadOp_Load;
		attachment.storeOp = WGPUStoreOp_Store;
		attachment.clearValue = {0, 0, 0, 1};
		WGPURenderPassDescriptor passDescriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
		passDescriptor.colorAttachmentCount = 1;
		passDescriptor.colorAttachments = &attachment;
		const auto pass = wgpuCommandEncoderBeginRenderPass(encoder, &passDescriptor);
		for (std::size_t i = 0; i < draws.size(); ++i)
		{
			const auto& draw = *draws[i];
			const auto found = m_pipelines.find(draw.shader);
			if (found == m_pipelines.end())
			{
				continue;
			}
			std::array<WGPUBindGroupEntry, 2> entries{};
			entries[0].binding = 0;
			entries[0].buffer = uniformBuffer;
			entries[0].offset = i * kAlignment;
			entries[0].size = sizeof(CustomPassUniform);
			entries[1].binding = 1;
			entries[1].buffer = dataBuffer;
			entries[1].offset = dataOffsets[i];
			entries[1].size = std::max<std::size_t>(draw.data.size(), 1) * sizeof(glm::vec4);
			WGPUBindGroupDescriptor binding = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
			binding.layout = m_layout;
			binding.entryCount = entries.size();
			binding.entries = entries.data();
			const auto group = wgpuDeviceCreateBindGroup(device, &binding);
			frame->bindings.push_back(group);
			wgpuRenderPassEncoderSetPipeline(pass, found->second[hdrTarget ? 0 : 1]);
			wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
			wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
		}
		wgpuRenderPassEncoderEnd(pass);
		wgpuRenderPassEncoderRelease(pass);
	}
} // namespace aether::webgpu
