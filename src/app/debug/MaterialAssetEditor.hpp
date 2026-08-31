#pragma once

#include <string>

#include "material/MaterialSerializer.hpp"

namespace aether
{
	class World;
}

namespace aether::app
{
	class LayerContext;
}

namespace aether::editor
{
	// Editing state for one open material asset.
	//
	// Held by whichever panel is showing it, so a slider drag mutates one in-memory copy and
	// the file is written once the drag ends rather than on every frame.
	struct MaterialAssetEditState
	{
		std::string path;
		MaterialPresetSpec spec;
		// The material as it currently is ON DISK. A write only happens when `spec` actually
		// differs from this, so an ImGui "changed" that carries no change - a widget rebuilt
		// under the cursor, a panel moving beneath a slider - cannot silently rewrite the
		// asset. It did exactly that once, turning a roughness of 0.85 into 0.04.
		MaterialPresetSpec saved;
		bool loaded = false;
		bool dirty = false;
		std::string error;
		int linkedCount = 0;
	};

	// Draws the material's own fields and saves them back to the asset, then pushes the change
	// into every entity linked to it.
	//
	// Shared by the Inspector and the Material Editor window: a second copy of this would be
	// the fourth place in this feature where the same logic was duplicated, and the previous
	// three had already drifted apart by the time anyone noticed.
	void DrawMaterialAssetEditor(app::LayerContext& context, World& world, const std::string& assetPath, MaterialAssetEditState& state);

	// True when two materials carry the same values. Used to decide whether anything actually
	// changed - both for enabling Save and for deciding the preview needs rebuilding.
	[[nodiscard]] bool MaterialSpecEquals(const MaterialPresetSpec& a, const MaterialPresetSpec& b);

	// Re-applies a material asset to every entity linked to it, leaving each entity's
	// overridden fields alone. Returns how many entities were updated.
	int PropagateMaterialAsset(app::LayerContext& context, World& world, const std::string& assetPath);
} // namespace aether::editor
