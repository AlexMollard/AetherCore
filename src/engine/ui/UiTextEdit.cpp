#include "ui/UiTextEdit.hpp"

#include <algorithm>

namespace aether::ui
{
	namespace
	{
		bool IsPrintableAscii(char c)
		{
			const auto u = static_cast<unsigned char>(c);
			return u >= 0x20 && u <= 0x7E;
		}

		bool PassesContentType(char c, TextContentType type)
		{
			const bool digit = c >= '0' && c <= '9';
			const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			switch (type)
			{
			case TextContentType::Integer:
				return digit || c == '-';
			case TextContentType::Decimal:
				return digit || c == '-' || c == '.';
			case TextContentType::Alphanumeric:
				return digit || alpha;
			case TextContentType::Host:
				// '-' is legal in DNS labels (my-host.local); ':' carries the port or an IPv6 literal.
				return digit || alpha || c == '.' || c == ':' || c == '-';
			case TextContentType::Any:
				break;
			}
			return true;
		}

		int ClampIndex(int i, std::size_t size)
		{
			return std::clamp(i, 0, static_cast<int>(size));
		}

		bool IsWordChar(char c)
		{
			const bool digit = c >= '0' && c <= '9';
			const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
			return digit || alpha || c == '_';
		}

		float GlyphAdvance(const FontAsset& font, char c, float scale)
		{
			const GlyphMeta* glyph = font.Find(static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
			return glyph != nullptr ? glyph->advance * scale : 0.f;
		}
	} // namespace

	int SelectionBegin(const TextEditState& s)
	{
		return std::min(ClampIndex(s.caret, s.text.size()), ClampIndex(s.selectionAnchor, s.text.size()));
	}

	int SelectionEnd(const TextEditState& s)
	{
		return std::max(ClampIndex(s.caret, s.text.size()), ClampIndex(s.selectionAnchor, s.text.size()));
	}

	bool HasSelection(const TextEditState& s)
	{
		return SelectionBegin(s) != SelectionEnd(s);
	}

	std::string SelectedText(const TextEditState& s)
	{
		const int begin = SelectionBegin(s);
		return s.text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(SelectionEnd(s) - begin));
	}

	void ClearSelection(TextEditState& s)
	{
		s.selectionAnchor = s.caret;
	}

	void SelectAll(TextEditState& s)
	{
		s.selectionAnchor = 0;
		s.caret = static_cast<int>(s.text.size());
	}

	std::string FilterInsert(const TextEditState& s, const TextEditLimits& limits, std::string_view chars)
	{
		std::string out;
		out.reserve(chars.size());
		for (const char c: chars)
		{
			if (!IsPrintableAscii(c) || !PassesContentType(c, limits.contentType))
			{
				continue;
			}
			if (!limits.allowedChars.empty() && limits.allowedChars.find(c) == std::string::npos)
			{
				continue;
			}
			out.push_back(c);
		}

		if (limits.maxLength > 0)
		{
			// The selection is about to be replaced, so its length is budget, not spend.
			const int selected = SelectionEnd(s) - SelectionBegin(s);
			const int remaining = limits.maxLength - (static_cast<int>(s.text.size()) - selected);
			if (remaining <= 0)
			{
				return {};
			}
			if (static_cast<int>(out.size()) > remaining)
			{
				out.resize(static_cast<std::size_t>(remaining));
			}
		}
		return out;
	}

	bool InsertText(TextEditState& s, const TextEditLimits& limits, std::string_view chars)
	{
		const std::string accepted = FilterInsert(s, limits, chars);
		if (accepted.empty())
		{
			return false;
		}

		DeleteSelection(s);
		s.caret = ClampIndex(s.caret, s.text.size());
		s.text.insert(static_cast<std::size_t>(s.caret), accepted);
		s.caret += static_cast<int>(accepted.size());
		ClearSelection(s);
		return true;
	}

	bool DeleteSelection(TextEditState& s)
	{
		if (!HasSelection(s))
		{
			return false;
		}
		const int begin = SelectionBegin(s);
		const int end = SelectionEnd(s);
		s.text.erase(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
		s.caret = begin;
		ClearSelection(s);
		return true;
	}

	std::string DisplayText(std::string_view text, bool password)
	{
		return password ? std::string(text.size(), '*') : std::string{text};
	}

	int WordBoundary(std::string_view text, int from, int dir)
	{
		const int size = static_cast<int>(text.size());
		int i = std::clamp(from, 0, size);
		if (dir < 0)
		{
			while (i > 0 && !IsWordChar(text[static_cast<std::size_t>(i - 1)]))
			{
				--i;
			}
			while (i > 0 && IsWordChar(text[static_cast<std::size_t>(i - 1)]))
			{
				--i;
			}
			return i;
		}
		while (i < size && !IsWordChar(text[static_cast<std::size_t>(i)]))
		{
			++i;
		}
		while (i < size && IsWordChar(text[static_cast<std::size_t>(i)]))
		{
			++i;
		}
		return i;
	}

	void MoveCaret(TextEditState& s, CaretMove move, bool extendSelection)
	{
		const int size = static_cast<int>(s.text.size());
		const bool hadSelection = HasSelection(s);
		int caret = ClampIndex(s.caret, s.text.size());

		switch (move)
		{
		case CaretMove::Left:
			// Collapse rather than step, so an arrow after a selection lands on its edge.
			caret = (hadSelection && !extendSelection) ? SelectionBegin(s) : std::max(caret - 1, 0);
			break;
		case CaretMove::Right:
			caret = (hadSelection && !extendSelection) ? SelectionEnd(s) : std::min(caret + 1, size);
			break;
		case CaretMove::WordLeft:
			caret = WordBoundary(s.text, caret, -1);
			break;
		case CaretMove::WordRight:
			caret = WordBoundary(s.text, caret, 1);
			break;
		case CaretMove::Home:
			caret = 0;
			break;
		case CaretMove::End:
			caret = size;
			break;
		}

		s.caret = caret;
		if (!extendSelection)
		{
			ClearSelection(s);
		}
	}

	bool DeleteBackward(TextEditState& s, bool wholeWord)
	{
		if (DeleteSelection(s))
		{
			return true;
		}
		s.caret = ClampIndex(s.caret, s.text.size());
		if (s.caret == 0)
		{
			return false;
		}
		const int begin = wholeWord ? WordBoundary(s.text, s.caret, -1) : s.caret - 1;
		s.text.erase(static_cast<std::size_t>(begin), static_cast<std::size_t>(s.caret - begin));
		s.caret = begin;
		ClearSelection(s);
		return true;
	}

	bool DeleteForward(TextEditState& s, bool wholeWord)
	{
		if (DeleteSelection(s))
		{
			return true;
		}
		s.caret = ClampIndex(s.caret, s.text.size());
		if (s.caret >= static_cast<int>(s.text.size()))
		{
			return false;
		}
		const int end = wholeWord ? WordBoundary(s.text, s.caret, 1) : s.caret + 1;
		s.text.erase(static_cast<std::size_t>(s.caret), static_cast<std::size_t>(end - s.caret));
		ClearSelection(s);
		return true;
	}

	void SelectWordAt(TextEditState& s, int index)
	{
		const int i = ClampIndex(index, s.text.size());
		// Step right first so an index sitting on a word start still selects that word.
		const int end = WordBoundary(s.text, i, 1);
		s.selectionAnchor = WordBoundary(s.text, end, -1);
		s.caret = end;
	}

	float TextWidth(const FontAsset& font, std::string_view text, float pixelSize)
	{
		if (font.bakeSize <= 0.f)
		{
			return 0.f;
		}
		const float scale = pixelSize / font.bakeSize;
		float width = 0.f;
		for (const char c: text)
		{
			width += GlyphAdvance(font, c, scale);
		}
		return width;
	}

	float CaretToPixelX(const FontAsset& font, std::string_view text, float pixelSize, int caret)
	{
		const int clamped = std::clamp(caret, 0, static_cast<int>(text.size()));
		return TextWidth(font, text.substr(0, static_cast<std::size_t>(clamped)), pixelSize);
	}

	int CaretFromPixelX(const FontAsset& font, std::string_view text, float pixelSize, float localX)
	{
		if (font.bakeSize <= 0.f || text.empty())
		{
			return 0;
		}
		const float scale = pixelSize / font.bakeSize;
		float x = 0.f;
		for (std::size_t i = 0; i < text.size(); ++i)
		{
			const float advance = GlyphAdvance(font, text[i], scale);
			// Past the halfway point of a glyph the caret belongs after it.
			if (localX < x + advance * 0.5f)
			{
				return static_cast<int>(i);
			}
			x += advance;
		}
		return static_cast<int>(text.size());
	}

	void ScrollToCaret(TextEditState& s, const FontAsset& font, float pixelSize, float innerWidth, bool password)
	{
		const std::string display = DisplayText(s.text, password);
		const float total = TextWidth(font, display, pixelSize);
		if (total <= innerWidth)
		{
			s.scrollX = 0.f; // it all fits; never leave the view scrolled
			return;
		}

		const float caretX = CaretToPixelX(font, display, pixelSize, s.caret);
		s.scrollX = std::clamp(s.scrollX, caretX - innerWidth, caretX);
		// Never scroll past the end of the text, and never before its start.
		s.scrollX = std::clamp(s.scrollX, 0.f, total - innerWidth);
	}
} // namespace aether::ui
