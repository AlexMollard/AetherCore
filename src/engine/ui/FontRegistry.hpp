#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "ui/FontAsset.hpp"

namespace aether::ui
{
	// Shapes `text` into per-glyph quads inside `boxRect` (x,y,w,h, output px,
	// top-left origin, y down). pixelSize is scaled against font.bakeSize.
	// hAlign: 0 Left, 1 Center, 2 Right. vAlign: 0 Top, 1 Middle, 2 Bottom.
	// Pure function - no GPU/World access, fully unit-testable.
	[[nodiscard]] std::vector<ShapedGlyph> ShapeText(const FontAsset& font, std::string_view text, float pixelSize, glm::vec4 boxRect, bool wrap, int hAlign, int vAlign);

	// Loads and caches fonts baked by AssetPacker's "bake-font" subcommand
	// (resources/fonts/<name>-Regular.fontmeta + .fontatlas, mounted under
	// assets://fonts/). Parses glyph metrics on the CPU; the atlas texture's
	// bindless slot is filled in by the GPU-owning caller (UiRenderer) after
	// Load returns - see FontAsset::atlasBindlessSlot.
	class FontRegistry
	{
	public:
		// Returns the cached/newly-parsed asset, or nullptr on read/parse failure.
		// name is the font family stem, e.g. "Roboto" -> "Roboto-Regular.fontmeta".
		const FontAsset* Load(std::string_view name);

		[[nodiscard]] const FontAsset* Get(std::string_view name) const;
		[[nodiscard]] FontAsset* GetMutable(std::string_view name);

		// Test-only: inject a hand-built FontAsset (e.g. a mono test font) without
		// touching the VFS or GPU.
		void InjectForTest(std::string name, FontAsset asset);

	private:
		std::unordered_map<std::string, FontAsset> m_fonts;
	};
} // namespace aether::ui
