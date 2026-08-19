// The runtime stages one AetherGame.dll beside itself and loads it unconditionally. For a
// published package that is right - the package has one project and the staged assembly is
// its own. Point a DEV build tree at a project with --project and it is wrong: the settings
// and scenes come from that project, the scripts come from whatever the engine staged, and
// the game boots the correct world running none of its own code.
//
// Nothing about that looks broken. Correct window, correct scene, right entity count, no
// error - and every menu dead to mouse, keyboard and gamepad alike, because the scripts that
// would have answered were never in the assembly.
//
// Two signals that look obvious are both wrong, and each is pinned by a test below:
//   - "staged outside the project" - the EDITOR's correct path also stages outside it, into
//     one directory shared by every project.
//   - "data/project.pak beside the executable means published" - a dev build tree that has
//     ever been packed into has one of those too.
// What actually distinguishes them is whether the staged file is one this project built.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "RuntimeProjectSettings.hpp"

using namespace aether::app;

namespace
{
	struct Layout
	{
		std::filesystem::path root;

		explicit Layout(const std::string& tag)
		{
			root = std::filesystem::temp_directory_path() / ("aether_scriptasm_" + tag);
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "project" / "scripts");
			std::filesystem::create_directories(root / "build" / "data" / "scripts" / "managed");
		}

		~Layout()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		[[nodiscard]] std::filesystem::path Project() const { return root / "project"; }
		[[nodiscard]] std::filesystem::path ExeDir() const { return root / "build"; }
		[[nodiscard]] std::filesystem::path Staged() const { return root / "build" / "data" / "scripts" / "managed"; }

		void GiveProjectScripts() const { Write(Project() / "scripts" / "AetherGame.csproj", "<Project/>"); }
		void MakePublishedPackage() const { Write(ExeDir() / "data" / "project.pak", "pak"); }

		// The engine's stub is a different size from a real game's assembly - 18 KB against
		// 101 KB in the run this was found in.
		void StageAssembly(const std::string& body) const { Write(Staged() / "AetherGame.dll", body); }

		void ProjectBuilt(const std::string& config, const std::string& body) const
		{
			Write(Project() / "Builds" / "Intermediate" / "managed" / "bin" / "AetherGame" / config / "AetherGame.dll", body);
		}

		static void Write(const std::filesystem::path& file, const std::string& body)
		{
			std::filesystem::create_directories(file.parent_path());
			std::ofstream out(file, std::ios::binary);
			out << body;
		}
	};

	RuntimeScriptAssemblyInputs In(const Layout& l)
	{
		return {l.Project(), l.ExeDir(), l.Staged()};
	}
} // namespace

// The configuration that cost a debugging session.
TEST_CASE("A staged assembly the project never built is flagged")
{
	const Layout layout("mismatch");
	layout.GiveProjectScripts();
	layout.StageAssembly("engine stub");
	layout.ProjectBuilt("debug", "the real game's much larger assembly");

	CHECK(ProjectScriptsCannotBeLoaded(In(layout)));
}

TEST_CASE("A staged assembly this project built is not flagged")
{
	const Layout layout("match");
	layout.GiveProjectScripts();
	layout.StageAssembly("the real game's assembly");
	layout.ProjectBuilt("debug", "the real game's assembly");

	CHECK_FALSE(ProjectScriptsCannotBeLoaded(In(layout)));
}

// The editor builds each project and stages the result into a directory shared by every
// project - outside the project tree. That correct case must not be flagged, which is why
// "staged outside the project" cannot be the signal.
TEST_CASE("Staging outside the project is not itself a problem")
{
	const Layout layout("outside");
	layout.GiveProjectScripts();
	layout.StageAssembly("built by the editor from this project");
	layout.ProjectBuilt("release", "built by the editor from this project");

	CHECK_FALSE(ProjectScriptsCannotBeLoaded(In(layout)));
}

// A project that has never had its scripts built cannot possibly have them staged.
TEST_CASE("A project that never built its scripts is flagged")
{
	const Layout layout("neverbuilt");
	layout.GiveProjectScripts();
	layout.StageAssembly("engine stub");

	CHECK(ProjectScriptsCannotBeLoaded(In(layout)));
}

// The signal that looked obvious and is not: the dev build tree this was found in had a
// 4.9 MB data/project.pak sitting next to the executable, so treating that as "published"
// suppressed the warning in exactly the case that needed it.
TEST_CASE("A project.pak beside the executable does not suppress the warning")
{
	const Layout layout("packedtree");
	layout.GiveProjectScripts();
	layout.MakePublishedPackage();
	layout.StageAssembly("engine stub");
	layout.ProjectBuilt("debug", "the real game's much larger assembly");

	CHECK(ProjectScriptsCannotBeLoaded(In(layout)));
}

TEST_CASE("Naming no project is never flagged")
{
	const Layout layout("noproject");
	layout.GiveProjectScripts();
	layout.StageAssembly("whatever shipped");

	// Without AETHER_PROJECT_DIR the runtime was not asked for a particular project, so what
	// sits beside it is by definition the game. This is the published-package path.
	CHECK_FALSE(ProjectScriptsCannotBeLoaded({{}, layout.ExeDir(), layout.Staged()}));
}

TEST_CASE("A project with no scripts of its own is never flagged")
{
	const Layout layout("noscripts");
	layout.StageAssembly("engine stub");

	CHECK_FALSE(ProjectScriptsCannotBeLoaded(In(layout)));
}

TEST_CASE("Nothing staged at all is left to the caller to report")
{
	const Layout layout("nostage");
	layout.GiveProjectScripts();

	CHECK_FALSE(ProjectScriptsCannotBeLoaded(In(layout)));
}
