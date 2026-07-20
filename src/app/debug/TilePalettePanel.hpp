#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include "debug/DebugPanel.hpp"

namespace aether
{
	struct TileSetAsset;
}

namespace aether::editor
{
	struct TilePaintingState;

	// Tile authoring hub for the selected Tile Map entity: tileset browser with
	// atlas previews, paint tools, layer list, chunk-local undo/redo, and asset
	// saving. Painting itself happens in the viewport (HandleTilePainting).
	class TilePalettePanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Tile Palette";
		}

		void OnImGui(app::LayerContext& context) override;
		void OnDetach(app::LayerContext& context) override;

	private:
		[[nodiscard]] std::uint64_t AcquirePreviewTexture(app::LayerContext& context, const std::string& texturePath);
		void ReleasePreviews(app::LayerContext& context);

		// Live "what the next click paints" swatch: the selected tile with the
		// active flips applied, or an erase/picker affordance for those tools.
		void DrawBrushPreview(app::LayerContext& context, const TileSetAsset& tileSet, const TilePaintingState& state);

		struct Preview
		{
			std::uint64_t imguiId = 0;
			std::int32_t width = 0;
			std::int32_t height = 0;
		};
		std::unordered_map<std::string, Preview> m_previews; // texture path -> registered preview
		char m_atlasInput[512] = {};
		char m_layerName[128] = {};
		std::string m_status;
		bool m_statusError = false;
	};
} // namespace aether::editor
