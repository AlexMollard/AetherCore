#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "editor/AutosaveService.hpp"
#include "editor/EditorProjectContext.hpp"
#include "io/FileUtil.hpp"

namespace
{
	using namespace aether;

	// A throwaway project layout on disk: root/scenes plus the .aether/recovery the
	// service writes into.
	struct TempProject
	{
		std::filesystem::path root;
		app::EditorProjectContext ctx;

		explicit TempProject(const std::string& tag)
		{
			root = std::filesystem::temp_directory_path() / ("aether_autosave_" + tag + "_" + std::to_string(std::filesystem::hash_value(tag)));
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "scenes");
			std::filesystem::create_directories(root / ".aether" / "recovery");
			ctx.root = root;
			ctx.scenesDir = root / "scenes";
			ctx.projectFile = root / "ProjectSettings.toml";
			ctx.name = tag;
			ctx.loaded = true;
		}

		~TempProject()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		[[nodiscard]] std::filesystem::path Scene(const std::string& name) const { return ctx.scenesDir / (name + ".scene.toml"); }
		[[nodiscard]] std::filesystem::path Recovery(const std::string& name) const { return root / ".aether" / "recovery" / (name + ".scene.toml"); }
	};

	// Minimal but genuinely parseable scene text.
	std::string SceneText(const std::string& entityName)
	{
		return "[scene]\nkind = '2d'\nname = 'T'\nversion = 16\n\n[[entities]]\nname = '" + entityName + "'\nparent = -1\nposition = [ 0.0, 0.0, 0.0 ]\n";
	}

	void Touch(const std::filesystem::path& path, const std::string& text)
	{
		REQUIRE(io::file_util::WriteText(path, text).has_value());
	}
} // namespace

TEST_CASE("A recovery copy newer than its scene is offered")
{
	const TempProject p("newer");
	Touch(p.Scene("Level"), SceneText("Saved"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	Touch(p.Recovery("Level"), SceneText("Unsaved"));

	const auto found = editor::AutosaveService::FindRecoverable(p.ctx);
	REQUIRE(found.size() == 1);
	CHECK(found[0].sceneName == "Level");
}

// The opposite case matters just as much: after a real save the scene file holds
// everything, so offering an older copy back would invite overwriting good work with
// stale work.
TEST_CASE("A recovery copy older than its scene is not offered")
{
	const TempProject p("older");
	Touch(p.Recovery("Level"), SceneText("Stale"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	Touch(p.Scene("Level"), SceneText("Saved"));

	CHECK(editor::AutosaveService::FindRecoverable(p.ctx).empty());
}

TEST_CASE("Restore promotes the recovery copy over the scene file")
{
	const TempProject p("restore");
	Touch(p.Scene("Level"), SceneText("Saved"));
	Touch(p.Recovery("Level"), SceneText("Recovered"));

	std::string error;
	REQUIRE(editor::AutosaveService::Restore(p.ctx, "Level", error));
	CHECK(error.empty());

	const auto text = io::file_util::ReadText(p.Scene("Level"));
	REQUIRE(text.has_value());
	CHECK(text->find("Recovered") != std::string::npos);
}

// The property that must never regress. A recovery copy is written by a process that
// may have died mid-write; promoting an unparseable one over a merely-out-of-date scene
// would destroy the very work this service exists to protect.
TEST_CASE("Restore refuses an unparseable recovery copy and leaves the scene alone")
{
	const TempProject p("corrupt");
	const std::string original = SceneText("Saved");
	Touch(p.Scene("Level"), original);
	Touch(p.Recovery("Level"), "[scene\nthis is = = not toml\n");

	std::string error;
	CHECK_FALSE(editor::AutosaveService::Restore(p.ctx, "Level", error));
	CHECK_FALSE(error.empty());

	const auto text = io::file_util::ReadText(p.Scene("Level"));
	REQUIRE(text.has_value());
	CHECK(*text == original);
}

TEST_CASE("Restore reports a missing recovery copy rather than touching the scene")
{
	const TempProject p("missing");
	const std::string original = SceneText("Saved");
	Touch(p.Scene("Level"), original);

	std::string error;
	CHECK_FALSE(editor::AutosaveService::Restore(p.ctx, "Level", error));

	const auto text = io::file_util::ReadText(p.Scene("Level"));
	REQUIRE(text.has_value());
	CHECK(*text == original);
}

TEST_CASE("Discard removes the recovery copy")
{
	const TempProject p("discard");
	Touch(p.Recovery("Level"), SceneText("Unsaved"));
	REQUIRE(io::file_util::Exists(p.Recovery("Level")));

	editor::AutosaveService::Discard(p.ctx, "Level");
	CHECK_FALSE(io::file_util::Exists(p.Recovery("Level")));
}

// ── Newer is not the same as different ──────────────────────────────────────
//
// The first recovery prompt shown against a real project offered a copy holding no work
// at all. Autosave writes whenever the edit history is dirty, and that stays dirty after
// an undo has put the scene back to what is on disk - so a copy can be newer, and empty.
// A prompt that cries wolf is one people learn to dismiss unread.

namespace
{
	// The same scene, written the way a FILE holds it and the way a CAPTURE of that file
	// comes back out: different entity order, node ids the file never had, and whole
	// numbers spelled as ints rather than floats. Same scene, three ways to disagree.
	std::string SceneAsAuthored()
	{
		return "[scene]\nkind = '2d'\nname = 'T'\nversion = 16\n\n"
		       "[[entities]]\nname = 'Coin'\nparent = -1\nposition = [ 86, 6, 0 ]\n\n"
		       "[[entities]]\nname = 'Player'\nparent = -1\nposition = [ 4, 2, 0 ]\n";
	}

	std::string SceneAsCaptured()
	{
		return "[scene]\nkind = '2d'\nname = 'T'\nversion = 16\n\n"
		       "[[entities]]\nname = 'Player'\nnode = 7071919957272742274\nparent = -1\nposition = [ 4.0, 2.0, 0.0 ]\n\n"
		       "[[entities]]\nname = 'Coin'\nnode = 879492768829902653\nparent = -1\nposition = [ 86.0, 6.0, 0.0 ]\n";
	}
} // namespace

TEST_CASE("A copy that only differs by order, node ids and int-vs-float holds nothing new")
{
	const TempProject p("identical");
	Touch(p.Scene("Level1"), SceneAsAuthored());
	Touch(p.Recovery("Level1"), SceneAsCaptured());

	CHECK(editor::AutosaveService::HoldsNothingNew(p.Recovery("Level1"), p.Scene("Level1")));
}

TEST_CASE("A copy holding nothing new is not offered, however much newer it is")
{
	const TempProject p("notoffered");
	Touch(p.Scene("Level1"), SceneAsAuthored());
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	Touch(p.Recovery("Level1"), SceneAsCaptured());

	// Timestamp-wise this is exactly the case that used to be offered.
	CHECK(editor::AutosaveService::FindRecoverable(p.ctx).empty());
}

TEST_CASE("A copy with real changes is still offered")
{
	const TempProject p("realwork");
	Touch(p.Scene("Level1"), SceneAsAuthored());
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	// One entity moved: the thing a recovery copy exists to hand back.
	std::string moved = SceneAsCaptured();
	const std::size_t at = moved.find("[ 86.0, 6.0, 0.0 ]");
	REQUIRE(at != std::string::npos);
	moved.replace(at, std::string("[ 86.0, 6.0, 0.0 ]").size(), "[ 99.0, 6.0, 0.0 ]");
	Touch(p.Recovery("Level1"), moved);

	CHECK_FALSE(editor::AutosaveService::HoldsNothingNew(p.Recovery("Level1"), p.Scene("Level1")));
	const auto found = editor::AutosaveService::FindRecoverable(p.ctx);
	REQUIRE(found.size() == 1);
	CHECK(found[0].sceneName == "Level1");
}

TEST_CASE("An added or removed entity is a real change")
{
	const TempProject p("added");
	Touch(p.Scene("Level1"), SceneAsAuthored());
	Touch(p.Recovery("Level1"), SceneAsCaptured() + "\n[[entities]]\nname = 'Extra'\nparent = -1\nposition = [ 1.0, 1.0, 0.0 ]\n");
	CHECK_FALSE(editor::AutosaveService::HoldsNothingNew(p.Recovery("Level1"), p.Scene("Level1")));
}

TEST_CASE("An unreadable copy is never dropped as empty")
{
	// Cannot tell means offer it. Dropping a copy we failed to parse would silently bin
	// the work it was holding.
	const TempProject p("unreadable");
	Touch(p.Scene("Level1"), SceneAsAuthored());
	Touch(p.Recovery("Level1"), "this is not a scene {{{");
	CHECK_FALSE(editor::AutosaveService::HoldsNothingNew(p.Recovery("Level1"), p.Scene("Level1")));

	Touch(p.Recovery("Level1"), SceneAsCaptured());
	CHECK_FALSE(editor::AutosaveService::HoldsNothingNew(p.Recovery("Level1"), p.Scene("Missing")));
}
