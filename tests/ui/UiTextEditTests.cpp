#include <doctest/doctest.h>

#include <string>

#include "platform/Input.hpp"

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
