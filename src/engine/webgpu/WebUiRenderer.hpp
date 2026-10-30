#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>
#include "ui/FontAsset.hpp"
#include "ui/UiFrameData.hpp"

namespace aether::webgpu
{
	struct WebUiFrame
	{
		std::vector<WGPUBuffer> buffers;
		std::vector<WGPUBindGroup> bindings;
		~WebUiFrame();
	};

	class WebUiRenderer
	{
	public:
		~WebUiRenderer();
		bool Initialize(WGPUDevice device, WGPUQueue queue);
		void Shutdown();
		bool UploadFont(WGPUDevice device, WGPUQueue queue, ui::FontAsset& font);
		bool AllocateFont(WGPUDevice device, ui::FontAsset& font);
		[[nodiscard]] WGPUTexture FontTexture(std::uint32_t slot) const;
		void ReleaseFont(std::uint32_t slot);
		bool UploadShader(WGPUDevice device, std::string_view stem, bool effect, bool glyph, std::string_view source);
		bool HasShader(std::string_view stem, bool effect, bool glyph) const;
		bool Encode(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView target,
		        unsigned width,
		        unsigned height,
		        const ui::UiFrameData& data,
		        const std::vector<WGPUTexture>& images,
		        WGPUSampler sampler,
		        std::unique_ptr<WebUiFrame>& frame);
		std::string error;

	private:
		WGPUShaderModule m_builtinShader = nullptr;
		WGPUBindGroupLayout m_layout = nullptr;
		WGPUPipelineLayout m_pipelineLayout = nullptr;
		WGPURenderPipeline m_default = nullptr;
		WGPUTexture m_dummyCurve = nullptr;
		std::vector<WGPUTexture> m_fonts;
		std::unordered_map<std::string, WGPURenderPipeline> m_materials, m_effects;
		WGPURenderPipeline CreatePipeline(WGPUDevice device, std::string_view source, bool effect);
	};
} // namespace aether::webgpu
