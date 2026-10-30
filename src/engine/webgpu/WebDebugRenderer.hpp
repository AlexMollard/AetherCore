#pragma once

#include <memory>
#include <string>
#include <webgpu/webgpu.h>

namespace aether
{
	struct RenderFramePacket;
}

namespace aether::webgpu
{
	struct WebDebugFrame
	{
		WGPUBuffer vertices = nullptr, uniform = nullptr;
		WGPUBindGroup binding = nullptr;
		WGPUTexture depth = nullptr;
		WGPUTextureView depthView = nullptr;
		~WebDebugFrame();
	};

	class WebDebugRenderer
	{
	public:
		~WebDebugRenderer();
		bool Initialize(WGPUDevice device);
		void Shutdown();
		bool Encode(
		        WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, WGPUTextureView color, unsigned width, unsigned height, const RenderFramePacket& packet, WGPUTextureView depth, bool sampledDepth, std::unique_ptr<WebDebugFrame>& frame);
		std::string error;

	private:
		WGPURenderPipeline m_attachmentPipeline = nullptr, m_sampledPipeline = nullptr;
	};
} // namespace aether::webgpu
