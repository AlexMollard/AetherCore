#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

#include "io/PlatformPaths.hpp"
#include "io/FileUtil.hpp"
#include "EditorDragDrop.hpp"

TEST_CASE("Environment paths preserve native Unicode project names")
{
#ifdef _WIN32
	constexpr auto* kVariable = L"AETHER_TEST_NATIVE_PROJECT_PATH";
	const std::filesystem::path expected = L"D:/launch \u9879\u76ee \U0001F680";
	REQUIRE(_wputenv_s(kVariable, expected.c_str()) == 0);
	CHECK(aether::io::PlatformPaths::ReadEnvironmentPath("AETHER_TEST_NATIVE_PROJECT_PATH") == expected);
	const auto expectedBytes = expected.u8string();
	CHECK(aether::io::PlatformPaths::PathToUtf8(expected) == std::string(expectedBytes.begin(), expectedBytes.end()));
	REQUIRE(_wputenv_s(kVariable, L"") == 0);
	CHECK(aether::io::PlatformPaths::ReadEnvironmentPath("AETHER_TEST_NATIVE_PROJECT_PATH").empty());
#else
	const std::filesystem::path expected = "/tmp/launch \u9879\u76ee \U0001F680";
	REQUIRE(setenv("AETHER_TEST_NATIVE_PROJECT_PATH", expected.c_str(), 1) == 0);
	CHECK(aether::io::PlatformPaths::ReadEnvironmentPath("AETHER_TEST_NATIVE_PROJECT_PATH") == expected);
	const auto expectedBytes = expected.u8string();
	CHECK(aether::io::PlatformPaths::PathToUtf8(expected) == std::string(expectedBytes.begin(), expectedBytes.end()));
	REQUIRE(unsetenv("AETHER_TEST_NATIVE_PROJECT_PATH") == 0);
#endif
}

TEST_CASE("Missing Unicode project files return an error without throwing")
{
#ifdef _WIN32
	const auto missing = std::filesystem::temp_directory_path() / L"aether-missing-\u9879\u76ee-\U0001F680" / "AetherGame.slnx";
#else
	const auto missing = std::filesystem::temp_directory_path() / "aether-missing-\u9879\u76ee-\U0001F680" / "AetherGame.slnx";
#endif
	const auto result = aether::io::file_util::ReadText(missing);
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().message.find(aether::io::PlatformPaths::PathToUtf8(missing)) != std::string::npos);
}
TEST_CASE("Unicode project paths retain asset file classifications")
{
	using aether::editor::dragdrop::ClassifyFile;
	using aether::editor::dragdrop::FileKind;
#ifdef _WIN32
	const std::filesystem::path root = L"D:/launch \u9879\u76ee \U0001F680";
#else
	const std::filesystem::path root = "/tmp/launch \u9879\u76ee \U0001F680";
#endif
	CHECK(ClassifyFile(root / "main.scene.toml") == FileKind::Scene);
	CHECK(ClassifyFile(root / "sample.prefab.toml") == FileKind::Prefab);
	CHECK(ClassifyFile(root / "materials" / "plain.toml") == FileKind::Material);
	CHECK(ClassifyFile(root / "graph.materialgraph.toml") == FileKind::MaterialGraph);
}