#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "ui/FontAsset.hpp"

namespace aether::ui
{
	[[nodiscard]] std::vector<ShapedGlyph> ShapeText(const FontAsset& font, std::string_view text, float pixelSize, glm::vec4 boxRect, bool wrap, int hAlign, int vAlign);

	class FontRegistry
	{
	public:
		const FontAsset* Load(std::string_view name);

		[[nodiscard]] const FontAsset* Get(std::string_view name) const;
		[[nodiscard]] FontAsset* GetMutable(std::string_view name);

		void InjectForTest(std::string name, FontAsset asset);

	private:
		std::unordered_map<std::string, FontAsset> m_fonts;
	};
} // namespace aether::ui
