#pragma once

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

namespace aether
{
	struct RenderFramePacket;
}

namespace aether::webgpu
{
	struct WebLight2DFrame
	{
		WGPUBuffer uniform = nullptr, lights = nullptr, occluders = nullptr;
		WGPUTexture mask = nullptr;
		WGPUTextureView maskView = nullptr;
		WGPUBindGroup lighting = nullptr, geometry = nullptr;
		std::vector<WGPUBindGroup> artwork;
		~WebLight2DFrame();
	};

	class WebLight2DRenderer
	{
	public:
		~WebLight2DRenderer();
		bool Initialize(WGPUDevice device);
		[[nodiscard]] std::expected<bool, std::string> Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures, unsigned width, unsigned height) const;
		void Encode(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView hdrColor,
		        unsigned width,
		        unsigned height,
		        const RenderFramePacket& packet,
		        std::span<const WGPUTexture> textures,
		        std::unique_ptr<WebLight2DFrame>& frame);
		void Shutdown();
		std::string error;

	private:
		WGPURenderPipeline m_lighting = nullptr, m_occlusion = nullptr;
		WGPUBindGroupLayout m_lighting_layout = nullptr, m_geometry_layout = nullptr, m_artwork_layout = nullptr;
		WGPUSampler m_sampler = nullptr;
	};
} // namespace aether::webgpu
