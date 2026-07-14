// project.pak (AETHER_PROJECT_PAK / pak mode), must prepend that pak's

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "AssetPipeline.hpp"
#include "io/FileSystem.hpp"

using namespace aether;

namespace
{
	std::filesystem::path MakeTempDir(const std::string& tag)
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("ae_fs_shader_overlay_test_" + tag);
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		return dir;
	}

	void WriteFile(const std::filesystem::path& path, const std::string& contents)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		out << contents;
	}

	void SetEnv(const char* name, const std::filesystem::path& value)
	{
#ifdef _MSC_VER
		_putenv_s(name, value.string().c_str());
#else
		setenv(name, value.string().c_str(), 1);
#endif
	}

	void ClearEnv(const char* name)
	{
#ifdef _MSC_VER
		_putenv_s(name, "");
#else
		unsetenv(name);
#endif
	}
}

TEST_CASE("InitializeDefaultMounts prepends a shipped project.pak shader layer over engine.pak")
{
	if (io::FileSystem::IsInitialized())
	{
		io::FileSystem::Shutdown();
	}

	const std::filesystem::path engineRoot = MakeTempDir("engine_root");
	WriteFile(engineRoot / "shaders" / "common.spv", "ENGINE_COMMON");
	WriteFile(engineRoot / "shaders" / "onlyengine.spv", "ENGINE_ONLY");
	const std::filesystem::path enginePak = std::filesystem::temp_directory_path() / "ae_fs_shader_overlay_engine.pak";
	std::filesystem::remove(enginePak);
	REQUIRE(assetpipeline::PackDirectory(engineRoot, enginePak, {}).ok);

	const std::filesystem::path projectRoot = MakeTempDir("project_root");
	std::filesystem::create_directories(projectRoot);
	const std::filesystem::path projectShaders = MakeTempDir("project_shaders");
	WriteFile(projectShaders / "common.spv", "PROJECT_COMMON");
	WriteFile(projectShaders / "onlyproject.spv", "PROJECT_ONLY");
	const std::filesystem::path projectPak = std::filesystem::temp_directory_path() / "ae_fs_shader_overlay_project.pak";
	std::filesystem::remove(projectPak);
	REQUIRE(assetpipeline::PackDirectory(projectRoot, projectPak, {.shaderSpirvDir = projectShaders}).ok);

	SetEnv("AETHER_ENGINE_PAK", enginePak);
	SetEnv("AETHER_PROJECT_PAK", projectPak);

	io::FileSystem::InitializeDefaultMounts();

	CHECK(io::FileSystem::Exists("shaders://common.spv"));
	const auto common = io::FileSystem::ReadFileText("shaders://common.spv");
	REQUIRE(common.has_value());
	CHECK(*common == "PROJECT_COMMON");

	const auto projectOnly = io::FileSystem::ReadFileText("shaders://onlyproject.spv");
	REQUIRE(projectOnly.has_value());
	CHECK(*projectOnly == "PROJECT_ONLY");

	const auto engineOnly = io::FileSystem::ReadFileText("shaders://onlyengine.spv");
	REQUIRE(engineOnly.has_value());
	CHECK(*engineOnly == "ENGINE_ONLY");

	const auto glob = io::FileSystem::Glob("shaders://*.spv");
	REQUIRE(glob.has_value());
	CHECK(glob->size() == 3);

	io::FileSystem::Shutdown();
	ClearEnv("AETHER_ENGINE_PAK");
	ClearEnv("AETHER_PROJECT_PAK");
	ClearEnv("AETHER_ENGINE_MODE");
	ClearEnv("AETHER_PROJECT_MODE");
}
