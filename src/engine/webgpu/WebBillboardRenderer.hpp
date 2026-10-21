#pragma once

#include <array>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>

#include "rendering/Render2DFrameData.hpp"

namespace aether
{
	struct RenderFramePacket;
}

namespace aether::webgpu
{
	struct WebBillboardFrame
	{
		std::vector<WGPUBuffer> buffers;
		std::vector<WGPUBindGroup> bindings;
		std::unordered_map<std::uint32_t, WGPUBindGroup> textureBindings;
		std::vector<BillboardParticleInstance> particles;
		bool prepared = false;
		~WebBillboardFrame();
	};

	class WebBillboardRenderer
	{
	public:
		~WebBillboardRenderer();
		bool Initialize(WGPUDevice device);
		[[nodiscard]] std::expected<void, std::string> Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures) const;
		void Encode(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView color,
		        unsigned width,
		        unsigned height,
		        const RenderFramePacket& packet,
		        std::span<const WGPUTexture> textures,
		        WGPUTextureView sceneDepth,
		        std::unique_ptr<WebBillboardFrame>& frame,
		        bool displaySpace);
		void Shutdown();
		std::string error;

	private:
		std::array<WGPURenderPipeline, 4> m_pipelines{};
		WGPUBindGroupLayout m_frame_layout = nullptr, m_texture_layout = nullptr;
		WGPUSampler m_sampler = nullptr;
		WGPUTexture m_white = nullptr;
	};
} // namespace aether::webgpu
