#pragma once

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
		// What the atlas looked like when it was last loaded or saved. Compared structurally
		// rather than by a flag set at each mutation site: regions are added, duplicated,
		// deleted, re-sliced and edited field by field, and a flag that misses one of those
		// reports "saved" over work that is not.
		[[nodiscard]] std::string AtlasSignature() const;
		[[nodiscard]] bool AtlasDirty() const;
		void DrawUnsavedAtlasPrompt(app::LayerContext& context);
		std::string m_savedAtlasSignature;
		std::string m_pendingAtlasPath;

		void SetSource(app::LayerContext& context, std::string path, bool deriveAtlasPath = true);
		void LoadAtlas(app::LayerContext& context, std::string path);
		void ImportAseprite(app::LayerContext& context, std::string path);
		void SaveAtlas(app::LayerContext& context);
		void ReleasePreview(app::LayerContext& context);
		void DrawPreview();
		void DrawRegionEditor();
		void ApplyPreset(int preset);

		std::string m_texturePath;
		std::string m_atlasPath;
		std::string m_asepritePath;
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
		int m_preset = 1;
		bool m_checkerboard = true;
		bool m_showPixelGrid = true;
		bool m_hasSlicePreview = false;
		std::string m_status;
		bool m_statusError = false;
	};
} // namespace aether::editor
