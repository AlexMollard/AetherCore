#pragma once

#include <array>
#include <memory>
#include <functional>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <webgpu/webgpu.h>
#include "gpu/PostEffectsPass.hpp"
#include "WebMeshRenderer.hpp"
#include "WebExposureRenderer.hpp"

namespace aether
{
	struct RenderFramePacket;
}

namespace aether::webgpu
{
	struct WebEffectImage
	{
		WGPUTexture texture = nullptr;
		WGPUTextureView view = nullptr;
		unsigned width = 0, height = 0;
		~WebEffectImage();
	};

	struct WebPostFrame
	{
		std::vector<std::unique_ptr<WebEffectImage>> images;
		std::vector<WGPUBuffer> buffers;
		std::vector<WGPUBindGroup> bindings;
		WebEffectImage *depth = nullptr, *normal = nullptr, *albedo = nullptr, *depthAttachment = nullptr;
		WebEffectImage *ao = nullptr, *aoDenoised = nullptr, *reflection = nullptr, *fog = nullptr, *dof = nullptr, *ldr = nullptr, *resolved = nullptr;
		std::array<WebEffectImage*, 5> bloom{};
		gpu::PostEffectsPlan plan;
		std::shared_ptr<WebExposureState> exposureState;
		std::unique_ptr<WebExposureFrame> exposureFrame;
		WebOpaqueInputs OpaqueInputs() const;
		~WebPostFrame();
	};

	class WebPostRenderer
	{
	public:
		~WebPostRenderer();
		bool Initialize(WGPUDevice device, WGPUQueue queue);
		void Shutdown();
		[[nodiscard]] std::expected<std::unique_ptr<WebPostFrame>, std::string> Allocate(WGPUDevice device, WGPUQueue queue, const gpu::PostEffectsPlan& plan, const RenderFramePacket& packet);
		void ClearOpaque(WGPUCommandEncoder encoder, const WebPostFrame& frame) const;
		void EncodeAmbient(WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, const RenderFramePacket& packet, WebPostFrame& frame, const WebShadowInputs& shadows);
		void EncodeScene(WGPUDevice device, WGPUQueue queue, WGPUCommandEncoder encoder, WGPUTextureView scene, const RenderFramePacket& packet, WebPostFrame& frame, const WebShadowInputs& shadows);
		void EncodeDisplay(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView scene,
		        WGPUTextureView canvas,
		        unsigned outputWidth,
		        unsigned outputHeight,
		        const RenderFramePacket& packet,
		        WebPostFrame& frame,
		        const WebShadowInputs& shadows,
		        const std::function<void(WGPUTextureView, unsigned, unsigned)>& afterTonemap = {});
		std::string error;

	private:
		struct ShaderFrame;
		enum class Pass : unsigned
		{
			BloomDown,
			BloomUp,
			Fxaa,
			Tonemap,
			Gtao,
			Denoise,
			Reflection,
			ReflectionComposite,
			Dof,
			Fog,
			FogComposite,
			Count
		};
		void Draw(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        Pass pass,
		        WGPUTextureView target,
		        unsigned width,
		        unsigned height,
		        const std::array<WGPUTextureView, 8>& textures,
		        const ShaderFrame& constants,
		        const void* push,
		        std::size_t pushSize,
		        const RenderFramePacket& packet,
		        WebPostFrame& frame,
		        bool load = false);
		ShaderFrame FrameConstants(const RenderFramePacket& packet, const WebPostFrame& frame, const WebShadowInputs& shadows) const;
		std::array<WGPURenderPipeline, static_cast<unsigned>(Pass::Count)> m_pipelines{};
		WGPUBindGroupLayout m_layout = nullptr;
		WGPUSampler m_sampler = nullptr;
		std::unique_ptr<WebEffectImage> m_white;
		WebExposureRenderer m_exposure;
		glm::mat4 m_previous{1};
		unsigned m_previous_width = 0, m_previous_height = 0;
		float m_previous_time = -1;
	};
} // namespace aether::webgpu
