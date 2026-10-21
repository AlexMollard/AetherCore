#pragma once

#include <memory>
#include <webgpu/webgpu.h>
#include "gpu/HdrPass.hpp"

namespace aether
{
	struct RenderFramePacket;
}

namespace aether::webgpu
{
	// One submitted frame owns every transient HDR resource until queue retirement.
	struct WebHdrFrame
	{
		WGPUTexture color = nullptr;
		WGPUTextureView view = nullptr;
		WGPUBuffer uniform = nullptr;
		WGPUBindGroup backgroundBindings = nullptr, compositeBindings = nullptr;
		~WebHdrFrame();
	};

	class WebHdrRenderer
	{
	public:
		~WebHdrRenderer();
		bool Initialize(WGPUDevice device);
		[[nodiscard]] std::expected<gpu::HdrPlan, std::string> Prepare(const RenderFramePacket& packet, unsigned width, unsigned height) const;
		[[nodiscard]] std::expected<std::unique_ptr<WebHdrFrame>, std::string> Allocate(WGPUDevice device, WGPUQueue queue, const gpu::HdrPlan& plan) const;
		void EncodeBackground(WGPUCommandEncoder encoder, const WebHdrFrame& frame) const;
		void EncodeComposite(WGPUCommandEncoder encoder, WGPUTextureView canvas, const WebHdrFrame& frame) const;
		void Shutdown();
		std::string error;

	private:
		WGPURenderPipeline m_background = nullptr, m_composite = nullptr;
		WGPUBindGroupLayout m_background_layout = nullptr, m_composite_layout = nullptr;
	};
} // namespace aether::webgpu
