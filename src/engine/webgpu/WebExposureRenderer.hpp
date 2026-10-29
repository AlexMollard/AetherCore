#pragma once

#include <cstdint>
#include <array>
#include <expected>
#include <memory>
#include <string>
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>

namespace aether
{
	struct RenderFramePacket;

	namespace gpu
	{
		struct PostEffectsSettings;
	}
} // namespace aether

namespace aether::webgpu
{
	struct WebExposureState
	{
		WGPUBuffer buffer = nullptr;
		std::array<WGPUBuffer, 3> histograms{};
		std::array<bool, 3> ready{};
		unsigned width = 0, height = 0;
		unsigned outputWidth = 0, outputHeight = 0;
		float renderScale = 1;
		std::uint64_t frameIndex = 0;
		~WebExposureState();
	};

	struct WebExposureFrame
	{
		std::shared_ptr<WebExposureState> state;
		std::array<bool, 3> ready{};
		WGPUBuffer uniform = nullptr;
		std::uint32_t slot = 0;
		std::uint64_t frameIndex = 0;
		WGPUBindGroup binding = nullptr;
		~WebExposureFrame();
	};

	class WebExposureRenderer
	{
	public:
		~WebExposureRenderer();
		bool Initialize(WGPUDevice device);
		[[nodiscard]] std::expected<std::shared_ptr<WebExposureState>, std::string> Prepare(WGPUDevice device, WGPUQueue queue, const RenderFramePacket& packet, unsigned width, unsigned height, unsigned outputWidth, unsigned outputHeight);
		void Encode(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView hdr,
		        unsigned width,
		        unsigned height,
		        const gpu::PostEffectsSettings& settings,
		        const std::shared_ptr<WebExposureState>& state,
		        std::unique_ptr<WebExposureFrame>& frame);
		void EncodeAdapt(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView hdr,
		        unsigned width,
		        unsigned height,
		        const gpu::PostEffectsSettings& settings,
		        const std::shared_ptr<WebExposureState>& state,
		        std::unique_ptr<WebExposureFrame>& frame);
		void Commit(const std::shared_ptr<WebExposureState>& state, const WebExposureFrame* frame);
		void Reset();
		void Shutdown();
		std::string error;

	private:
		WGPUComputePipeline m_histogram = nullptr, m_adapt = nullptr;
		WGPUBindGroupLayout m_layout = nullptr;
		WGPUSampler m_sampler = nullptr;
		std::shared_ptr<WebExposureState> m_state;
	};
} // namespace aether::webgpu
