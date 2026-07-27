#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aether::ui
{
	// Pure single-line text editing. No World, no Input, no GLFW - so every rule here is
	// unit-testable directly, the same way SliderQuantize is. UiTextBoxSystem is the only
	// caller; it just marshals ECS + Input into these.
	//
	// ASCII ONLY: ShapeText treats each char byte as a codepoint and the baked atlases are
	// ASCII, so multibyte input would render as mojibake rather than simply fail. Insertion
	// filters to printable ASCII 0x20-0x7E, which makes every index below a byte index that
	// is also a character index.

	enum class TextContentType : std::uint8_t
	{
		Any,
		Integer,
		Decimal,
		Alphanumeric,
		Host // network address: letters, digits, dots, colons - "192.168.0.1:7777", "localhost", "fe80::1"
	};

	struct TextEditState
	{
		std::string text;
		int caret = 0;           // insertion point, 0..text.size()
		int selectionAnchor = 0; // the fixed end of the selection; == caret means no selection
		float scrollX = 0.f;     // px of text scrolled off the left edge
	};

	struct TextEditLimits
	{
		TextContentType contentType = TextContentType::Any;
		std::string allowedChars; // non-empty = whitelist, ANDed with contentType
		int maxLength = 0;        // 0 = unlimited
	};

	[[nodiscard]] int SelectionBegin(const TextEditState& s);
	[[nodiscard]] int SelectionEnd(const TextEditState& s);
	[[nodiscard]] bool HasSelection(const TextEditState& s);
	[[nodiscard]] std::string SelectedText(const TextEditState& s);
	void ClearSelection(TextEditState& s);
	void SelectAll(TextEditState& s);

	// The subset of `chars` acceptable right now: printable ASCII, passing contentType and
	// allowedChars, truncated to whatever maxLength budget remains once the current selection
	// (which the insert would replace) is accounted for.
	[[nodiscard]] std::string FilterInsert(const TextEditState& s, const TextEditLimits& limits, std::string_view chars);

	// Each returns true when `text` actually changed, so the caller raises `changed` once.
	bool InsertText(TextEditState& s, const TextEditLimits& limits, std::string_view chars);
	bool DeleteSelection(TextEditState& s);

	// `text` with every character replaced by '*' when password is set.
	[[nodiscard]] std::string DisplayText(std::string_view text, bool password);
} // namespace aether::ui
