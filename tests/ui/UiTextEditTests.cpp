#include <doctest/doctest.h>

#include <string>

#include "platform/Input.hpp"
#include "ui/FontAsset.hpp"
#include "ui/UiTextEdit.hpp"

using namespace aether;

TEST_CASE("Clipboard falls back to an internal string with no window")
{
	Input input; // no Init(): windowless, as in unit tests

	CHECK(input.GetClipboardText().empty());

	input.SetClipboardText("192.168.0.1");
	CHECK(input.GetClipboardText() == "192.168.0.1");

	input.SetClipboardText("");
	CHECK(input.GetClipboardText().empty());
}

TEST_CASE("FilterInsert drops non-printable and non-ASCII input")
{
	ui::TextEditState s;
	const ui::TextEditLimits limits;

	CHECK(ui::FilterInsert(s, limits, "abc") == "abc");
	CHECK(ui::FilterInsert(s, limits, "a\nb\tc") == "abc");   // control chars dropped
	CHECK(ui::FilterInsert(s, limits, "caf\xC3\xA9") == "caf"); // UTF-8 continuation bytes dropped
}

TEST_CASE("FilterInsert honours the content type")
{
	ui::TextEditState s;

	ui::TextEditLimits ints;
	ints.contentType = ui::TextContentType::Integer;
	CHECK(ui::FilterInsert(s, ints, "12a3") == "123");

	ui::TextEditLimits dec;
	dec.contentType = ui::TextContentType::Decimal;
	CHECK(ui::FilterInsert(s, dec, "1.5x") == "1.5");

	ui::TextEditLimits alnum;
	alnum.contentType = ui::TextContentType::Alphanumeric;
	CHECK(ui::FilterInsert(s, alnum, "ab-12_") == "ab12");

	ui::TextEditLimits ip;
	ip.contentType = ui::TextContentType::Host;
	CHECK(ui::FilterInsert(s, ip, "192.168.0.1:7777") == "192.168.0.1:7777");
	CHECK(ui::FilterInsert(s, ip, "host name") == "hostname");
	CHECK(ui::FilterInsert(s, ip, "my-host.local:7777") == "my-host.local:7777"); // DNS label hyphen
	CHECK(ui::FilterInsert(s, ip, "fe80::1") == "fe80::1");                       // IPv6 literal
	CHECK(ui::FilterInsert(s, ip, "a/b?c") == "abc");                             // still filters
}

TEST_CASE("FilterInsert intersects contentType with allowedChars")
{
	ui::TextEditState s;
	ui::TextEditLimits limits;
	limits.contentType = ui::TextContentType::Integer;
	limits.allowedChars = "0123";

	CHECK(ui::FilterInsert(s, limits, "0123456789") == "0123");
	CHECK(ui::FilterInsert(s, limits, "1a2") == "12");
}

TEST_CASE("FilterInsert respects maxLength against the current text")
{
	ui::TextEditState s;
	s.text = "abc";
	s.caret = 3;
	s.selectionAnchor = 3;

	ui::TextEditLimits limits;
	limits.maxLength = 5;

	CHECK(ui::FilterInsert(s, limits, "de") == "de");
	CHECK(ui::FilterInsert(s, limits, "defg") == "de"); // truncated at the limit

	// A selection is about to be replaced, so its length is budget.
	s.selectionAnchor = 0;
	CHECK(ui::FilterInsert(s, limits, "defg") == "defg");

	// Already full with nothing selected: no budget at all, so everything is rejected.
	s.text = "abcde";
	s.caret = 5;
	s.selectionAnchor = 5;
	CHECK(ui::FilterInsert(s, limits, "f").empty());
}

TEST_CASE("InsertText inserts at the caret and advances it")
{
	ui::TextEditState s;
	const ui::TextEditLimits limits;

	CHECK(ui::InsertText(s, limits, "hello"));
	CHECK(s.text == "hello");
	CHECK(s.caret == 5);
	CHECK(s.selectionAnchor == 5);

	s.caret = 0;
	s.selectionAnchor = 0;
	CHECK(ui::InsertText(s, limits, ">"));
	CHECK(s.text == ">hello");
	CHECK(s.caret == 1);

	CHECK_FALSE(ui::InsertText(s, limits, "")); // nothing survived filtering
	CHECK(s.text == ">hello");
}

TEST_CASE("InsertText replaces the selection")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 6;
	s.caret = 11;

	const ui::TextEditLimits limits;
	CHECK(ui::InsertText(s, limits, "there"));
	CHECK(s.text == "hello there");
	CHECK(s.caret == 11);
	CHECK_FALSE(ui::HasSelection(s));
}

TEST_CASE("Selection helpers order the anchor and caret")
{
	ui::TextEditState s;
	s.text = "abcdef";
	s.selectionAnchor = 4;
	s.caret = 1;

	CHECK(ui::HasSelection(s));
	CHECK(ui::SelectionBegin(s) == 1);
	CHECK(ui::SelectionEnd(s) == 4);
	CHECK(ui::SelectedText(s) == "bcd");

	ui::ClearSelection(s);
	CHECK_FALSE(ui::HasSelection(s));
	CHECK(s.selectionAnchor == 1);

	ui::SelectAll(s);
	CHECK(ui::SelectionBegin(s) == 0);
	CHECK(ui::SelectionEnd(s) == 6);
	CHECK(s.caret == 6);
}

TEST_CASE("DeleteSelection removes the range and collapses the caret")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 5;
	s.caret = 11;

	CHECK(ui::DeleteSelection(s));
	CHECK(s.text == "hello");
	CHECK(s.caret == 5);
	CHECK_FALSE(ui::HasSelection(s));

	CHECK_FALSE(ui::DeleteSelection(s)); // nothing selected
}

TEST_CASE("DisplayText masks a password")
{
	CHECK(ui::DisplayText("secret", false) == "secret");
	CHECK(ui::DisplayText("secret", true) == "******");
	CHECK(ui::DisplayText("", true).empty());
}

// Mirrors MakeMonoFont in UiDrawBuilderTests: bakeSize 48, advance 24 -> at pixelSize 48 every
// glyph is exactly 24 px wide, so expected pixel positions are caret * 24.
static ui::FontAsset MakeTextEditFont()
{
	ui::FontAsset f;
	f.atlasBindlessSlot = 42;
	f.atlasWidth = f.atlasHeight = 128;
	f.ascent = 40;
	f.descent = 10;
	f.lineHeight = 50;
	f.bakeSize = 48;
	for (char c = 0x20; c > 0 && c <= 0x7E; ++c)
	{
		f.glyphs[static_cast<std::uint32_t>(c)] = ui::GlyphMeta{static_cast<std::uint32_t>(c), 0.f, 0.f, 0.1f, 0.1f, 20.f, 30.f, 0.f, 30.f, 24.f};
	}
	return f;
}

TEST_CASE("MoveCaret steps by character and clamps at the ends")
{
	ui::TextEditState s;
	s.text = "abc";
	s.caret = 1;
	s.selectionAnchor = 1;

	ui::MoveCaret(s, ui::CaretMove::Right, false);
	CHECK(s.caret == 2);
	ui::MoveCaret(s, ui::CaretMove::Right, false);
	ui::MoveCaret(s, ui::CaretMove::Right, false);
	CHECK(s.caret == 3); // clamped

	ui::MoveCaret(s, ui::CaretMove::Left, false);
	CHECK(s.caret == 2);
	ui::MoveCaret(s, ui::CaretMove::Home, false);
	CHECK(s.caret == 0);
	ui::MoveCaret(s, ui::CaretMove::Left, false);
	CHECK(s.caret == 0); // clamped
	ui::MoveCaret(s, ui::CaretMove::End, false);
	CHECK(s.caret == 3);
}

TEST_CASE("MoveCaret collapses a selection instead of stepping")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 2;
	s.caret = 7;

	ui::MoveCaret(s, ui::CaretMove::Left, false);
	CHECK(s.caret == 2); // collapses to the selection start, does not step to 6
	CHECK_FALSE(ui::HasSelection(s));

	s.selectionAnchor = 2;
	s.caret = 7;
	ui::MoveCaret(s, ui::CaretMove::Right, false);
	CHECK(s.caret == 7); // collapses to the selection end
	CHECK_FALSE(ui::HasSelection(s));
}

TEST_CASE("MoveCaret with extendSelection keeps the anchor")
{
	ui::TextEditState s;
	s.text = "hello";
	s.caret = 2;
	s.selectionAnchor = 2;

	ui::MoveCaret(s, ui::CaretMove::Right, true);
	CHECK(s.caret == 3);
	CHECK(s.selectionAnchor == 2);
	CHECK(ui::SelectedText(s) == "l");

	ui::MoveCaret(s, ui::CaretMove::End, true);
	CHECK(s.caret == 5);
	CHECK(s.selectionAnchor == 2);
	CHECK(ui::SelectedText(s) == "llo");
}

TEST_CASE("WordBoundary skips runs of word characters and separators")
{
	const std::string_view text = "hello big world";

	CHECK(ui::WordBoundary(text, 15, -1) == 10); // back over "world"
	CHECK(ui::WordBoundary(text, 10, -1) == 6);  // back over " big" -> start of "big"
	CHECK(ui::WordBoundary(text, 0, -1) == 0);   // clamped

	CHECK(ui::WordBoundary(text, 0, 1) == 5);    // forward over "hello"
	CHECK(ui::WordBoundary(text, 5, 1) == 9);    // forward over " big"
	CHECK(ui::WordBoundary(text, 15, 1) == 15);  // clamped
}

TEST_CASE("DeleteBackward and DeleteForward remove one character or one word")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.caret = 11;
	s.selectionAnchor = 11;

	CHECK(ui::DeleteBackward(s, false));
	CHECK(s.text == "hello worl");
	CHECK(s.caret == 10);

	CHECK(ui::DeleteBackward(s, true));
	CHECK(s.text == "hello ");
	CHECK(s.caret == 6);

	s.caret = 0;
	s.selectionAnchor = 0;
	CHECK_FALSE(ui::DeleteBackward(s, false)); // nothing to the left

	CHECK(ui::DeleteForward(s, false));
	CHECK(s.text == "ello ");
	CHECK(s.caret == 0);

	s.text = "abc";
	s.caret = 3;
	s.selectionAnchor = 3;
	CHECK_FALSE(ui::DeleteForward(s, false)); // nothing to the right
}

TEST_CASE("Delete keys remove the selection when there is one")
{
	ui::TextEditState s;
	s.text = "hello world";
	s.selectionAnchor = 0;
	s.caret = 6;

	CHECK(ui::DeleteBackward(s, false));
	CHECK(s.text == "world");
	CHECK(s.caret == 0);
}

TEST_CASE("Text measurement maps carets to pixels and back")
{
	const ui::FontAsset font = MakeTextEditFont();

	CHECK(ui::TextWidth(font, "abc", 48.f) == doctest::Approx(72.f));
	CHECK(ui::TextWidth(font, "", 48.f) == doctest::Approx(0.f));

	CHECK(ui::CaretToPixelX(font, "abc", 48.f, 0) == doctest::Approx(0.f));
	CHECK(ui::CaretToPixelX(font, "abc", 48.f, 2) == doctest::Approx(48.f));
	CHECK(ui::CaretToPixelX(font, "abc", 48.f, 3) == doctest::Approx(72.f));

	// Hit-testing snaps to the nearest gap between characters.
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, -5.f) == 0);
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, 10.f) == 0);
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, 14.f) == 1);
	CHECK(ui::CaretFromPixelX(font, "abc", 48.f, 500.f) == 3);
}

TEST_CASE("ScrollToCaret keeps the caret inside the visible window")
{
	const ui::FontAsset font = MakeTextEditFont();

	ui::TextEditState s;
	s.text = "abcdefghij"; // 240 px at 24 px/char
	s.caret = 10;
	s.selectionAnchor = 10;

	ui::ScrollToCaret(s, font, 48.f, 100.f, false);
	CHECK(s.scrollX == doctest::Approx(140.f)); // caret at 240 sits on the right edge

	s.caret = 0;
	ui::ScrollToCaret(s, font, 48.f, 100.f, false);
	CHECK(s.scrollX == doctest::Approx(0.f)); // scrolled back to reveal the start

	// Short text never scrolls, whatever the caret did before.
	s.text = "ab";
	s.caret = 2;
	s.scrollX = 90.f;
	ui::ScrollToCaret(s, font, 48.f, 100.f, false);
	CHECK(s.scrollX == doctest::Approx(0.f));
}

TEST_CASE("SelectWordAt selects the word under an index")
{
	ui::TextEditState s;
	s.text = "hello big world";

	ui::SelectWordAt(s, 7);
	CHECK(ui::SelectedText(s) == "big");
	CHECK(s.caret == 9);
	CHECK(s.selectionAnchor == 6);

	ui::SelectWordAt(s, 0);
	CHECK(ui::SelectedText(s) == "hello");
}

TEST_CASE("A synthetic key still produces a down-edge for a windowed Input")
{
	// Regression: SetSyntheticKey used to seed m_currKeys unconditionally. With a
	// window, Update() begins with `m_prevKeys = m_currKeys`, so that seed made prev
	// and curr both true and IsKeyPressed never fired for an injected key - silently
	// breaking every headless playtest that waits on a key PRESS rather than a hold.
	// Windowless callers still need the seed (they never call Update()), so the fix is
	// to gate it on there being no window. This pins the windowless half; the windowed
	// half is unobservable here because Update() dereferences the GLFW window.
	Input input; // no Init(): windowless

	input.SetSyntheticKey(static_cast<int>(Key::Space), true);
	CHECK(input.IsKeyDown(Key::Space));

	input.ClearSyntheticKeys();
	input.SetSyntheticKey(static_cast<int>(Key::Space), false);
	CHECK_FALSE(input.IsKeyDown(Key::Space));
}
