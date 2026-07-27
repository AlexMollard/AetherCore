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
} // namespace aether::ui
