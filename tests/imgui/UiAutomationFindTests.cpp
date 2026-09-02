// ui_query finds a widget, you feed what it showed you to ui_click, and ui_click says it
// does not exist. The two matched differently - query by substring, click exactly - so the
// labels query hands back (carrying ImGui's "###id" suffixes, icon glyphs and whatever the
// panel appended) were rejected by the tool query exists to feed.
#include <doctest/doctest.h>

#include <string>

#include "imgui/UiAutomation.hpp"

using aether::app::UiAutomation;
using aether::app::UiItem;

namespace
{
	// The registry is a singleton fed by ImGui's item hooks. Build a frame by hand and
	// swap it in; the swap also clears whatever the previous case left.
	struct Frame
	{
		unsigned int next = 1;

		Frame() { UiAutomation::Get().BeginFrameSwap(); }

		Frame& Item(const char* window, const char* label, bool clipped = false, bool clippedHorizontally = false)
		{
			const unsigned int id = next++;
			UiAutomation::Get().RecordItemAdd(id, 10.0f * static_cast<float>(id), 20.0f, 30.0f, 40.0f, window, clipped, clippedHorizontally);
			UiAutomation::Get().RecordItemInfo(id, label);
			return *this;
		}

		void Commit() { UiAutomation::Get().BeginFrameSwap(); }
	};

	std::string FindError(const char* window, const char* label)
	{
		std::string err;
		const auto item = UiAutomation::Get().FindItem(window, label, err);
		return item.has_value() ? std::string{} : err;
	}
} // namespace

TEST_CASE("A window name that ui_query would match also works for ui_click")
{
	// The real failure: the window is "Recover Unsaved Work###recoverScenes", query
	// accepted "Recover", and click refused it.
	Frame f;
	f.Item("Recover Unsaved Work###recoverScenes", "Decide later");
	f.Item("Viewport", "Play");
	f.Commit();

	std::string err;
	const auto hit = UiAutomation::Get().FindItem("Recover", "Decide later", err);
	REQUIRE_MESSAGE(hit.has_value(), err);
	CHECK(hit->window == "Recover Unsaved Work###recoverScenes");
}

TEST_CASE("A label that ui_query would match also works for ui_click")
{
	// The other one: the Project panel's button carries a leading icon glyph, so the
	// label is " Launcher", not "Launcher".
	Frame f;
	f.Item("Project", " Launcher");
	f.Commit();

	std::string err;
	const auto hit = UiAutomation::Get().FindItem("Project", "Launcher", err);
	REQUIRE_MESSAGE(hit.has_value(), err);
	CHECK(hit->label == " Launcher");
}

TEST_CASE("An exact label wins over the longer ones it is a prefix of")
{
	// "Save" must not become ambiguous just because "Save As..." is on the same menu.
	Frame f;
	f.Item("File", "Save");
	f.Item("File", "Save As...");
	f.Item("File", "Save & Return to Project Launcher");
	f.Commit();

	std::string err;
	const auto hit = UiAutomation::Get().FindItem("File", "Save", err);
	REQUIRE_MESSAGE(hit.has_value(), err);
	CHECK(hit->label == "Save");
}

TEST_CASE("A genuinely ambiguous match names the candidates")
{
	// "pass a window to disambiguate" is no help when the windows are exactly what the
	// caller cannot see from where they are standing.
	Frame f;
	f.Item("Scene", "Delete");
	f.Item("File Explorer", "Delete");
	f.Commit();

	const std::string err = FindError("", "Delete");
	REQUIRE_FALSE(err.empty());
	CHECK(err.find("ambiguous") != std::string::npos);
	CHECK(err.find("Scene") != std::string::npos);
	CHECK(err.find("File Explorer") != std::string::npos);
}

TEST_CASE("A label that matches nothing still reports that plainly")
{
	Frame f;
	f.Item("Viewport", "Play");
	f.Commit();

	const std::string err = FindError("Viewport", "Publish");
	CHECK(err.find("no widget labelled 'Publish'") != std::string::npos);
	CHECK(err.find("Viewport") != std::string::npos);
}

TEST_CASE("The window filter still narrows a label that appears in several")
{
	Frame f;
	f.Item("Scene", "Delete");
	f.Item("File Explorer", "Delete");
	f.Commit();

	std::string err;
	const auto hit = UiAutomation::Get().FindItem("File Explorer", "Delete", err);
	REQUIRE_MESSAGE(hit.has_value(), err);
	CHECK(hit->window == "File Explorer");
}

TEST_CASE("A clipped widget is recorded as clipped")
{
	// The flag is the only way a caller can tell a truncated label from a whole one: the
	// rect is the same either way, so a UI audit that reads rects alone sees nothing wrong.
	Frame frame;
	frame.Item("Inspector", "Whole").Item("Inspector", "Cut off", /*clipped=*/true);
	frame.Commit();

	bool sawWhole = false;
	bool sawCut = false;
	for (const UiItem& item: UiAutomation::Get().Snapshot())
	{
		if (item.label == "Whole")
		{
			sawWhole = true;
			CHECK_FALSE(item.clipped);
		}
		if (item.label == "Cut off")
		{
			sawCut = true;
			CHECK(item.clipped);
		}
	}
	CHECK(sawWhole);
	CHECK(sawCut);
}

TEST_CASE("Clipping records which axis it happened on")
{
	// Both are clipped, but only one is worth acting on. A row half-drawn at the bottom of a
	// scrolling list is what scrolling looks like; a control drawn past the side of a panel
	// is a control the user cannot reach.
	Frame frame;
	frame.Item("List", "Bottom row", /*clipped=*/true, /*clippedHorizontally=*/false);
	frame.Item("Toolbar", "Off the side", /*clipped=*/true, /*clippedHorizontally=*/true);
	frame.Commit();

	for (const UiItem& item: UiAutomation::Get().Snapshot())
	{
		if (item.label == "Bottom row")
		{
			CHECK(item.clipped);
			CHECK_FALSE(item.clippedHorizontally);
		}
		if (item.label == "Off the side")
		{
			CHECK(item.clipped);
			CHECK(item.clippedHorizontally);
		}
	}
}
