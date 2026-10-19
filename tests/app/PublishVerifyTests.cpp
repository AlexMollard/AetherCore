#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "editor/publish/PublishVerify.hpp"
#include "io/FileUtil.hpp"
#include "RuntimeProjectSettings.hpp"

using namespace aether;

namespace
{
	// A package that satisfies the file-presence invariants, so a test only has to break the
	// one thing it is about. The pak-content checks run last and cannot be satisfied by stub
	// files; tests that reach them assert on what they can.
	struct FakePackage
	{
		std::filesystem::path root;

		explicit FakePackage(const std::string& name)
		{
			root = std::filesystem::temp_directory_path() / ("aethercore_publishverify_" + name);
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
			for (const char* rel: {"AetherGame.exe",
			             "data/config/EngineSettings.toml",
			             "data/engine.pak",
			             "data/project.pak",
			             "data/scripts/managed/AetherCore.dll",
			             "data/scripts/managed/AetherCore.Interop.dll",
			             "data/scripts/managed/AetherCore.Interop.deps.json",
			             "data/scripts/managed/AetherCore.Interop.runtimeconfig.json",
			             "data/scripts/managed/AetherGame.dll",
			             "data/scripts/managed/AetherGame.deps.json"})
			{
				Write(rel, std::string_view(rel) == "data/config/EngineSettings.toml" ? "[app]\nstartupScene = \"main\"\nautoplay = true\n" : "x");
			}
		}

		~FakePackage()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		void Write(const std::string& rel, const std::string& contents) const
		{
			const std::filesystem::path path = root / rel;
			REQUIRE(io::file_util::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(io::file_util::WriteText(path, contents).has_value());
		}

		void Remove(const std::string& rel) const
		{
			std::error_code ec;
			std::filesystem::remove(root / rel, ec);
		}
	};
} // namespace

TEST_CASE("VerifyPublishedPackage names the missing file")
{
	const FakePackage package("missing");
	package.Remove("data/scripts/managed/AetherGame.dll");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("AetherGame.dll") != std::string::npos);
	CHECK_FALSE(issue->remediation.empty());
}

TEST_CASE("VerifyPublishedPackage rejects source files in the package")
{
	const FakePackage package("source");
	package.Write("data/scripts/PlayerController.cs", "class X {}");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("PlayerController.cs") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects a packer sidecar")
{
	const FakePackage package("sidecar");
	package.Write("data/project.pak.log", "packed 3 files");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("project.pak.log") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects the Aftermath DLL")
{
	const FakePackage package("aftermath");
	package.Write("GFSDK_Aftermath_Lib.x64.dll", "binary");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("Aftermath") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects the editor executable")
{
	const FakePackage package("editor");
	package.Write("Editor.exe", "binary");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	REQUIRE(issue.has_value());
	CHECK(issue->message.find("Editor.exe") != std::string::npos);
}

// A debug CRT DLL is NOT a verification failure. Build configuration is reported from
// AE_CONFIG_NAME instead, which a Debug publish cannot contradict.
TEST_CASE("VerifyPublishedPackage accepts a debug CRT DLL")
{
	const FakePackage package("debugcrt");
	package.Write("msvcp140d.dll", "binary");
	package.Write("ucrtbased.dll", "binary");

	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

	// It still fails on the stub engine.pak, but never because of the CRT DLLs.
	if (issue.has_value())
	{
		CHECK(issue->message.find("msvcp140d") == std::string::npos);
		CHECK(issue->message.find("ucrtbased") == std::string::npos);
	}
}

TEST_CASE("IsPrunablePublishedFile covers build leftovers but not shipped content")
{
	CHECK(editor::IsPrunablePublishedFile("AetherGame.pdb"));
	CHECK(editor::IsPrunablePublishedFile("Engine.lib"));
	CHECK(editor::IsPrunablePublishedFile("Editor.ilk"));
	CHECK_FALSE(editor::IsPrunablePublishedFile("AetherGame.exe"));
	CHECK_FALSE(editor::IsPrunablePublishedFile("data/engine.pak"));
}

TEST_CASE("IsForbiddenPublishedFile covers source that must never ship")
{
	CHECK(editor::IsForbiddenPublishedFile("Player.cs"));
	CHECK(editor::IsForbiddenPublishedFile("AetherGame.csproj"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("AetherGame.dll"));
}

TEST_CASE("VerifyPublishedPackage requires the Rust native module selected by baked settings")
{
	const FakePackage package("rust_missing");
	package.Write("data/config/EngineSettings.toml", "[scripting]\nlanguage = \"rust\"\n");
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find(app::ResolveRuntimeGameplayModule({}, app::GameplayLanguage::Rust).generic_string()) != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage accepts a Rust payload without managed gameplay dependencies")
{
	const FakePackage package("rust_payload");
	package.Write("data/config/EngineSettings.toml", "[scripting]\nlanguage = \"rust\"\n");
	for (const char* file: {"AetherCore.dll", "AetherCore.Interop.dll", "AetherCore.Interop.deps.json", "AetherCore.Interop.runtimeconfig.json", "AetherGame.dll", "AetherGame.deps.json"})
	{
		package.Remove(std::string("data/scripts/managed/") + file);
	}
	package.Write(app::ResolveRuntimeGameplayModule({}, app::GameplayLanguage::Rust).generic_string(), "module");
	const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
	REQUIRE(issue.has_value());
	CHECK(issue->message.find("engine.pak") != std::string::npos);
	CHECK(issue->message.find("managed") == std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects invalid baked gameplay language")
{
	const FakePackage package("invalid_language");
	for (const std::string& text: {std::string("[scripting]\nlanguage = \"python\"\n"), std::string("[scripting]\nlanguage = 12\n"), std::string("not toml")})
	{
		package.Write("data/config/EngineSettings.toml", text);
		const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");
		REQUIRE(issue.has_value());
		CHECK(issue->message.find("scripting") != std::string::npos);
		CHECK_FALSE(issue->remediation.empty());
	}
}

TEST_CASE("IsForbiddenPublishedFile covers Rust sources and crate build artifacts")
{
	CHECK(editor::IsForbiddenPublishedFile("player.rs"));
	CHECK(editor::IsForbiddenPublishedFile("Cargo.toml"));
	CHECK(editor::IsForbiddenPublishedFile("Cargo.lock"));
	CHECK(editor::IsForbiddenPublishedFile("libaether_gameplay.rlib"));
	CHECK(editor::IsForbiddenPublishedFile("libaether_gameplay.rmeta"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("aether_gameplay.dll"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("libaether_gameplay.so"));
	CHECK_FALSE(editor::IsForbiddenPublishedFile("EngineSettings.toml"));
}
