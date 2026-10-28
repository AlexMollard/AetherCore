#include "WebExposureRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include "gpu/PostEffectsPass.hpp"
#include "rendering/RenderFramePacket.hpp"

namespace aether::webgpu
{
	namespace
	{
		WGPUBuffer Buffer(WGPUDevice device, std::uint64_t size, WGPUBufferUsage usage)
		{
			WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
			descriptor.size = size;
			descriptor.usage = usage;
			return wgpuDeviceCreateBuffer(device, &descriptor);
		}

		struct MeterSettings
		{
			std::uint32_t width, height, meterWidth = 256, meterHeight = 144;
			float key, speed;
			std::uint32_t enabled = 1, pad = 0;
		};

		static_assert(sizeof(MeterSettings) == 32);
	} // namespace

	WebExposureState::~WebExposureState()
	{
		if (buffer)
		{
			wgpuBufferDestroy(buffer);
			wgpuBufferRelease(buffer);
		}
		for (auto histogram: histograms)
		{
			if (histogram)
			{
				wgpuBufferDestroy(histogram);
				wgpuBufferRelease(histogram);
			}
		}
	}

	WebExposureFrame::~WebExposureFrame()
	{
		if (binding)
		{
			wgpuBindGroupRelease(binding);
		}
		if (uniform)
		{
			wgpuBufferDestroy(uniform);
			wgpuBufferRelease(uniform);
		}
	}

	WebExposureRenderer::~WebExposureRenderer()
	{
		Shutdown();
	}

	void WebExposureRenderer::Reset()
	{
		m_state.reset();
	}

	void WebExposureRenderer::Shutdown()
	{
		Reset();
		if (m_histogram)
		{
			wgpuComputePipelineRelease(m_histogram);
			m_histogram = nullptr;
		}
		if (m_adapt)
		{
			wgpuComputePipelineRelease(m_adapt);
			m_adapt = nullptr;
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
	}

	bool WebExposureRenderer::Initialize(WGPUDevice device)
	{
		Shutdown();
		error.clear();
		std::ifstream file("/assets/exposure.wgsl");
		const std::string code(std::istreambuf_iterator<char>(file), {});
		if (code.empty())
		{
			error = "WEB_EXPOSURE_SHADER: missing exposure.wgsl";
			return false;
		}
		WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
		source.code = {code.data(), code.size()};
		WGPUShaderModuleDescriptor moduleDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		moduleDescriptor.nextInChain = &source.chain;
		const auto module = wgpuDeviceCreateShaderModule(device, &moduleDescriptor);
		if (!module)
		{
			error = "WEB_EXPOSURE_SHADER: module creation failed";
			return false;
		}

		std::array<WGPUBindGroupLayoutEntry, 5> entries;
		for (std::size_t i = 0; i < entries.size(); ++i)
		{
			entries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
			entries[i].binding = static_cast<std::uint32_t>(i);
			entries[i].visibility = WGPUShaderStage_Compute;
		}
		entries[0].texture.sampleType = WGPUTextureSampleType_Float;
		entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
		entries[2].buffer.type = WGPUBufferBindingType_Storage;
		entries[2].buffer.minBindingSize = 256 * sizeof(std::uint32_t);
		entries[3].buffer.type = WGPUBufferBindingType_Storage;
		entries[3].buffer.minBindingSize = sizeof(float);
		entries[4].buffer.type = WGPUBufferBindingType_Uniform;
		entries[4].buffer.minBindingSize = sizeof(MeterSettings);
		WGPUBindGroupLayoutDescriptor layoutDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		layoutDescriptor.entryCount = entries.size();
		layoutDescriptor.entries = entries.data();
		m_layout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
		WGPUPipelineLayoutDescriptor pipelineLayoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pipelineLayoutDescriptor.bindGroupLayoutCount = 1;
		pipelineLayoutDescriptor.bindGroupLayouts = &m_layout;
		const auto pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDescriptor);
		for (auto [name, destination]: {std::pair{"histogram", &m_histogram}, std::pair{"adapt", &m_adapt}})
		{
			WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
			descriptor.layout = pipelineLayout;
			descriptor.compute.module = module;
			descriptor.compute.entryPoint = {name, std::strlen(name)};
			*destination = wgpuDeviceCreateComputePipeline(device, &descriptor);
		}
		wgpuPipelineLayoutRelease(pipelineLayout);
		wgpuShaderModuleRelease(module);
		WGPUSamplerDescriptor samplerDescriptor = WGPU_SAMPLER_DESCRIPTOR_INIT;
		samplerDescriptor.addressModeU = samplerDescriptor.addressModeV = samplerDescriptor.addressModeW = WGPUAddressMode_ClampToEdge;
		samplerDescriptor.magFilter = samplerDescriptor.minFilter = WGPUFilterMode_Linear;
		m_sampler = wgpuDeviceCreateSampler(device, &samplerDescriptor);
		if (!m_layout || !m_histogram || !m_adapt || !m_sampler)
		{
			error = "WEB_EXPOSURE_PIPELINE: creation failed";
			Shutdown();
			return false;
		}
		return true;
	}

	std::expected<std::shared_ptr<WebExposureState>, std::string> WebExposureRenderer::Prepare(WGPUDevice device, WGPUQueue queue, const RenderFramePacket& packet, unsigned width, unsigned height, std::uint64_t sceneGeneration)
	{
		if (!width || !height || width > 8192 || height > 8192 || !std::isfinite(packet.elapsedTime))
		{
			return std::unexpected("WEB_EXPOSURE_FRAME: invalid finite time or extent budget");
		}
		if (!std::isfinite(packet.postEffects.exposureKey) || !std::isfinite(packet.postEffects.exposureSpeed) || packet.postEffects.exposureKey <= 0 || packet.postEffects.exposureSpeed < 0)
		{
			return std::unexpected("WEB_EXPOSURE_FRAME: invalid adaptation settings before allocation");
		}
		const auto viewProj = packet.proj * packet.view;
		for (int column = 0; column < 4; ++column)
		{
			for (int row = 0; row < 4; ++row)
			{
				if (!std::isfinite(viewProj[column][row]))
				{
					return std::unexpected("WEB_EXPOSURE_FRAME: non-finite camera");
				}
			}
		}
		const bool enabled = packet.postEffects.autoExposure;
		if (!m_state || m_state->width != width || m_state->height != height || m_state->sceneGeneration != sceneGeneration || m_state->enabled != enabled)
		{
			auto state = std::make_shared<WebExposureState>();
			state->buffer = Buffer(device, 16, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc);
			if (!state->buffer)
			{
				return std::unexpected("WEB_EXPOSURE_ALLOCATION: state buffer failed");
			}
			const std::array<float, 4> initial{1, 0, 0, 0};
			wgpuQueueWriteBuffer(queue, state->buffer, 0, initial.data(), sizeof(initial));
			const std::array<std::uint32_t, 256> empty{};
			for (auto& histogram: state->histograms)
			{
				histogram = Buffer(device, sizeof(empty), WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
				if (!histogram)
				{
					return std::unexpected("WEB_EXPOSURE_ALLOCATION: histogram ring failed");
				}
				wgpuQueueWriteBuffer(queue, histogram, 0, empty.data(), sizeof(empty));
			}
			state->width = width;
			state->height = height;
			state->generation = ++m_generation;
			state->sceneGeneration = sceneGeneration;
			state->enabled = enabled;
			m_state = std::move(state);
		}
		m_state->lastTime = packet.elapsedTime;
		m_state->enabled = enabled;
		m_state->lastViewProj = viewProj;
		m_state->frameIndex = packet.temporalFrameIndex == UINT64_MAX ? packet.frameIndex : packet.temporalFrameIndex;
		return m_state;
	}

	void WebExposureRenderer::EncodeAdapt(WGPUDevice device,
	        WGPUQueue queue,
	        WGPUCommandEncoder encoder,
	        WGPUTextureView hdr,
	        unsigned width,
	        unsigned height,
	        const gpu::PostEffectsSettings& settings,
	        const std::shared_ptr<WebExposureState>& state,
	        std::unique_ptr<WebExposureFrame>& frame)
	{
		if (!state || !state->buffer || state->width != width || state->height != height || !hdr || !std::isfinite(settings.exposureKey) || !std::isfinite(settings.exposureSpeed) || settings.exposureKey <= 0 || settings.exposureSpeed < 0)
		{
			error = "WEB_EXPOSURE_FRAME: invalid state, source or adaptation settings";
			return;
		}
		auto owned = std::make_unique<WebExposureFrame>();
		owned->state = state;
		owned->frameIndex = state->frameIndex;
		owned->slot = static_cast<std::uint32_t>(state->frameIndex % state->histograms.size());
		if (!settings.autoExposure)
		{
			frame = std::move(owned);
			return;
		}
		owned->uniform = Buffer(device, sizeof(MeterSettings), WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
		if (!owned->uniform)
		{
			error = "WEB_EXPOSURE_ALLOCATION: transient meter buffers failed";
			return;
		}
		const MeterSettings constants{.width = width, .height = height, .key = settings.exposureKey, .speed = settings.exposureSpeed};
		wgpuQueueWriteBuffer(queue, owned->uniform, 0, &constants, sizeof(constants));
		std::array<WGPUBindGroupEntry, 5> entries;
		for (std::size_t i = 0; i < entries.size(); ++i)
		{
			entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
			entries[i].binding = static_cast<std::uint32_t>(i);
		}
		entries[0].textureView = hdr;
		entries[1].sampler = m_sampler;
		entries[2].buffer = state->histograms[owned->slot];
		entries[2].size = 256 * sizeof(std::uint32_t);
		entries[3].buffer = state->buffer;
		entries[3].size = 16;
		entries[4].buffer = owned->uniform;
		entries[4].size = sizeof(MeterSettings);
		WGPUBindGroupDescriptor groupDescriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		groupDescriptor.layout = m_layout;
		groupDescriptor.entryCount = entries.size();
		groupDescriptor.entries = entries.data();
		owned->binding = wgpuDeviceCreateBindGroup(device, &groupDescriptor);
		if (!owned->binding)
		{
			error = "WEB_EXPOSURE_BINDING: meter binding failed";
			return;
		}
		if (state->ready[owned->slot])
		{
			WGPUComputePassDescriptor descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
			const auto pass = wgpuCommandEncoderBeginComputePass(encoder, &descriptor);
			wgpuComputePassEncoderSetBindGroup(pass, 0, owned->binding, 0, nullptr);
			wgpuComputePassEncoderSetPipeline(pass, m_adapt);
			wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
			wgpuComputePassEncoderEnd(pass);
			wgpuComputePassEncoderRelease(pass);
			state->ready[owned->slot] = false;
		}
		frame = std::move(owned);
	}

	void WebExposureRenderer::Encode(
	        WGPUDevice, WGPUQueue, WGPUCommandEncoder encoder, WGPUTextureView, unsigned, unsigned, const gpu::PostEffectsSettings& settings, const std::shared_ptr<WebExposureState>& state, std::unique_ptr<WebExposureFrame>& frame)
	{
		if (!settings.autoExposure)
		{
			return;
		}
		if (!frame || frame->state != state || !frame->binding)
		{
			error = "WEB_EXPOSURE_ORDER: EncodeAdapt must prepare the retained frame before tonemapping";
			return;
		}
		if (frame->frameIndex % 6 != 0)
		{
			return;
		}
		wgpuCommandEncoderClearBuffer(encoder, state->histograms[frame->slot], 0, 256 * sizeof(std::uint32_t));
		WGPUComputePassDescriptor descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
		const auto pass = wgpuCommandEncoderBeginComputePass(encoder, &descriptor);
		wgpuComputePassEncoderSetBindGroup(pass, 0, frame->binding, 0, nullptr);
		wgpuComputePassEncoderSetPipeline(pass, m_histogram);
		wgpuComputePassEncoderDispatchWorkgroups(pass, 144, 1, 1);
		wgpuComputePassEncoderEnd(pass);
		wgpuComputePassEncoderRelease(pass);
		state->ready[frame->slot] = true;
	}

} // namespace aether::webgpu
