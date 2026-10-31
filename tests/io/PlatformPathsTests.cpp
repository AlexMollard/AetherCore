#include <doctest/doctest.h>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "io/PlatformPaths.hpp"
#include "io/FileUtil.hpp"
#include "EditorDragDrop.hpp"

TEST_CASE("UI layout persistence resolves into user data without a working-directory fallback")
{
	using aether::io::PlatformPaths;
	const auto user = std::filesystem::temp_directory_path() / "ae-ui-user";
	CHECK(PlatformPaths::ResolveUserUiLayoutPath(user, "Editor") == user / "ui" / "Editor.ini");
	CHECK(PlatformPaths::ResolveUserUiLayoutPath(user, "Launcher") == user / "ui" / "Launcher.ini");
	CHECK(PlatformPaths::ResolveUserUiLayoutPath({}, "Editor").empty());
	CHECK(PlatformPaths::ResolveUserUiLayoutPath("relative-user", "Editor").empty());
	CHECK(PlatformPaths::ResolveUserUiLayoutPath(user, "").empty());
	CHECK(PlatformPaths::ResolveUserUiLayoutPath(user, "../Editor").empty());
	CHECK(PlatformPaths::ResolveUserUiLayoutPath(user, "nested/Editor").empty());
}

TEST_CASE("Delivered product markers enable isolation without a qualification environment")
{
	using aether::io::PlatformPaths;
	const auto root = std::filesystem::temp_directory_path() / ("ae-product-marker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup
	{
		std::filesystem::path root;
		~Cleanup()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup{root};
	std::filesystem::create_directories(root);
	CHECK_FALSE(PlatformPaths::IsIsolatedProduct(root, ""));
	CHECK_FALSE(PlatformPaths::IsIsolatedProduct(root, "0"));
	CHECK(PlatformPaths::IsIsolatedProduct(root, "1"));
	std::ofstream(root / "agent-session") << "agent snapshot";
	CHECK_FALSE(PlatformPaths::IsIsolatedProduct(root, ""));
	std::filesystem::create_directory(root / "BUILD_PROVENANCE.json");
	CHECK_FALSE(PlatformPaths::IsIsolatedProduct(root, ""));
	std::filesystem::remove(root / "BUILD_PROVENANCE.json");
	std::ofstream(root / "BUILD_PROVENANCE.json") << "{}";
	CHECK(PlatformPaths::IsIsolatedProduct(root, ""));
	CHECK(PlatformPaths::IsIsolatedProduct(root, "0"));
	std::filesystem::remove(root / "BUILD_PROVENANCE.json");
	std::ofstream(root / "aether-product") << "AetherCore packaged product\n";
	CHECK(PlatformPaths::IsIsolatedProduct(root, ""));
	CHECK(PlatformPaths::IsIsolatedProduct(root, "0"));
	std::filesystem::remove(root / "aether-product");
	CHECK_FALSE(PlatformPaths::IsIsolatedProduct(root, ""));
}

TEST_CASE("Isolated products resolve delivered paths without environment or developer fallbacks")
{
	using aether::io::PlatformPaths;
	const auto previousMode = PlatformPaths::ReadEnvironmentVariable("AETHER_ISOLATED_PRODUCT");
	const auto previousOverride = PlatformPaths::ReadEnvironmentVariable("AETHER_TEST_BUNDLE_OVERRIDE");
	const auto setVariable = [](const char* name, const std::string& value)
	{
#ifdef _WIN32
		return _putenv_s(name, value.c_str());
#else
		return value.empty() ? unsetenv(name) : setenv(name, value.c_str(), 1);
#endif
	};
	struct Restore
	{
		std::string mode;
		std::string overrideValue;
		std::filesystem::path scratch;
		~Restore()
		{
#ifdef _WIN32
			_putenv_s("AETHER_ISOLATED_PRODUCT", mode.c_str());
			_putenv_s("AETHER_TEST_BUNDLE_OVERRIDE", overrideValue.c_str());
#else
			if (mode.empty()) { unsetenv("AETHER_ISOLATED_PRODUCT"); }
			else { setenv("AETHER_ISOLATED_PRODUCT", mode.c_str(), 1); }
			if (overrideValue.empty()) { unsetenv("AETHER_TEST_BUNDLE_OVERRIDE"); }
			else { setenv("AETHER_TEST_BUNDLE_OVERRIDE", overrideValue.c_str(), 1); }
#endif
			std::error_code ignored;
			std::filesystem::remove_all(scratch, ignored);
		}
	} restore{previousMode, previousOverride, std::filesystem::temp_directory_path() / ("ae-isolated-product-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
	std::filesystem::create_directories(restore.scratch);
	const auto external = restore.scratch / "developer-tool";
	std::ofstream(external) << "external fixture";
	REQUIRE(setVariable("AETHER_TEST_BUNDLE_OVERRIDE", external.string()) == 0);
	REQUIRE(setVariable("AETHER_ISOLATED_PRODUCT", "0") == 0);
	CHECK(PlatformPaths::ResolveBundlePath("AETHER_TEST_BUNDLE_OVERRIDE", external.string(), "missing-delivered-path") == external);
	CHECK(PlatformPaths::ResolveToolExecutable("AETHER_TEST_BUNDLE_OVERRIDE", external.string(), "missing-delivered-tool") == external);
	REQUIRE(setVariable("AETHER_ISOLATED_PRODUCT", "1") == 0);
	CHECK(PlatformPaths::ResolveBundlePath("AETHER_TEST_BUNDLE_OVERRIDE", external.string(), "missing-delivered-path").empty());
	CHECK(PlatformPaths::ResolveToolExecutable("AETHER_TEST_BUNDLE_OVERRIDE", external.string(), "missing-delivered-tool").empty());
	const auto executableDir = std::filesystem::canonical(PlatformPaths::GetExecutableDir());
	const auto executableName = PlatformPaths::GetExecutableName() +
#ifdef _WIN32
		".exe";
#else
		"";
#endif
	CHECK(PlatformPaths::ResolveBundlePath("AETHER_TEST_BUNDLE_OVERRIDE", external.string(), ".") == executableDir);
	CHECK(PlatformPaths::ResolveToolExecutable("AETHER_TEST_BUNDLE_OVERRIDE", external.string(), executableName) == executableDir / executableName);
	CHECK(PlatformPaths::ResolveBundlePath("", "", external.string()).empty());
	CHECK(PlatformPaths::ResolveToolExecutable("", "", external.string()).empty());
	const auto escaped = std::filesystem::relative(external, executableDir).generic_string();
	CHECK(PlatformPaths::ResolveBundlePath("", "", escaped).empty());
	CHECK(PlatformPaths::ResolveToolExecutable("", "", escaped).empty());
}

TEST_CASE("Explicit user data directory isolates settings and rejects relative paths")
{
	using aether::io::PlatformPaths;
	const auto previous = PlatformPaths::ReadEnvironmentPath("AETHER_USER_DATA_DIR");
	struct RestoreEnvironment
	{
		std::filesystem::path value;
		~RestoreEnvironment()
		{
#ifdef _WIN32
			_wputenv_s(L"AETHER_USER_DATA_DIR", value.c_str());
#else
			if (value.empty()) { unsetenv("AETHER_USER_DATA_DIR"); }
			else { setenv("AETHER_USER_DATA_DIR", value.c_str(), 1); }
#endif
		}
	} restore{previous};
	const auto isolated = std::filesystem::temp_directory_path() /
		("aether-user-data-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#ifdef _WIN32
	REQUIRE(_wputenv_s(L"AETHER_USER_DATA_DIR", isolated.c_str()) == 0);
#else
	REQUIRE(setenv("AETHER_USER_DATA_DIR", isolated.c_str(), 1) == 0);
#endif
	CHECK(PlatformPaths::GetUserConfigDir() == isolated);
#ifdef _WIN32
	REQUIRE(_wputenv_s(L"AETHER_USER_DATA_DIR", L"relative-user-data") == 0);
#else
	REQUIRE(setenv("AETHER_USER_DATA_DIR", "relative-user-data", 1) == 0);
#endif
	CHECK(PlatformPaths::GetUserConfigDir().empty());
#ifdef _WIN32
	REQUIRE(_wputenv_s(L"AETHER_USER_DATA_DIR", previous.c_str()) == 0);
#else
	REQUIRE(previous.empty() ? unsetenv("AETHER_USER_DATA_DIR") == 0 : setenv("AETHER_USER_DATA_DIR", previous.c_str(), 1) == 0);
#endif
	std::error_code ec;
	std::filesystem::remove(isolated, ec);
}

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
