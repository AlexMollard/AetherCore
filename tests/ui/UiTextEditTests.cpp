#include <doctest/doctest.h>

#include <string>

#include "platform/Input.hpp"
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
