#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "assets/SpriteAtlasAsset.hpp"
#include "debug/DebugPanel.hpp"
#include "material/TextureHandle.hpp"

namespace aether::editor
{
	class SpriteSlicerPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Sprite Slicer";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnDetach(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;

	private:
		void LoadSource(app::LayerContext& context);
		void ReleasePreview(app::LayerContext& context);
		void DrawPreview();
		void DrawRegionEditor();
		void ApplyPreset(int preset);

		std::array<char, 512> m_texturePath{};
		std::array<char, 512> m_atlasPath{};
		std::array<char, 512> m_asepritePath{};
		SpriteAtlasAsset m_atlas;
		SpriteAtlasAsset m_slicePreview;
		SpriteAtlasReimportDiagnostics m_diagnostics;
		std::vector<std::uint8_t> m_rgbaPixels;
		TextureHandle m_previewTexture{};
		std::uint64_t m_previewTextureId = 0;
		std::int32_t m_textureWidth = 1;
		std::int32_t m_textureHeight = 1;
		std::int32_t m_selectedRegion = -1;
		float m_zoom = 2.0f;
		glm::vec2 m_pan{0.0f};
		int m_preset = 1;
		bool m_checkerboard = true;
		bool m_showPixelGrid = true;
		bool m_hasSlicePreview = false;
		std::string m_status;
		bool m_statusError = false;
	};
} // namespace aether::editor
