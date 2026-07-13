#pragma once

#include <string>
#include <unordered_map>

#include "debug/DebugPanel.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::editor
{
	class TextureInspectorPanel final : public DebugPanel
	{
	public:
		~TextureInspectorPanel() override;

		std::string_view GetName() const override
		{
			return "TextureInspector";
		}

		void OnDetach(app::LayerContext& context) override;
		void OnUpdate(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;
		void OnRenderTargetsInvalidated(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		static const char* FormatName(gpu::Format format) noexcept;
		static std::string ImageUsageText(gpu::ImageUsage usage);
		static std::string ImageAspectText(gpu::ImageAspect aspect);

		void ReleaseTextures(app::LayerContext& context);

		std::unordered_map<std::uint32_t, std::uint64_t> m_textureInspectorTextureIds;
		std::uint32_t m_selectedTextureBits = 0;
		int m_texturePreviewChannel = 0;
		float m_texturePreviewZoom = 1.0f;
		bool m_texturePreviewCheckerboard = true;
		bool m_texturePreviewFit = false;
		char m_texSearch[128] = {};
		int m_texUsageFilter = 0;
		float m_previewExposure = 1.0f;
		bool m_previewTonemap = false;
		bool m_useGpuPreview = false; // opt-in: GPU channel/exposure pass vs direct display
		std::uint64_t m_previewTextureId = 0;
	};
} // namespace aether::editor
