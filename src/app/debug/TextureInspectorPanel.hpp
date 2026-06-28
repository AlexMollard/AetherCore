#pragma once

#include <string>
#include <unordered_map>

#include "debug/DebugPanel.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::app
{
	class TextureInspectorPanel final : public DebugPanel
	{
	public:
		~TextureInspectorPanel() override;

		std::string_view GetName() const override
		{
			return "TextureInspector";
		}

		void OnDetach(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;

	private:
		static const char* FormatName(gpu::Format format) noexcept;
		static std::string ImageUsageText(gpu::ImageUsage usage);
		static std::string ImageAspectText(gpu::ImageAspect aspect);

		void ReleaseTextures(LayerContext& context);

		std::unordered_map<std::uint32_t, std::uint64_t> m_textureInspectorTextureIds;
		std::uint32_t m_selectedTextureBits = 0;
		int m_texturePreviewChannel = 0;
		float m_texturePreviewZoom = 1.0f;
		bool m_texturePreviewCheckerboard = true;
	};
} // namespace aether::app
