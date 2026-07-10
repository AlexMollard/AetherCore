// End-to-end proof for the Phase 4 shipped shaders:// overlay wiring:
// FileSystem::InitializeDefaultMounts(), when project:// resolves to a real
// project.pak (AETHER_PROJECT_PAK / pak mode), must prepend that pak's
// "shaders/" prefix as the highest-priority shaders:// layer - so a project
// shader of the same name overrides the engine shader, a project-only shader
// still resolves, and an engine-only shader still falls through. No editor
// involved: this is exactly the code path a shipped GameRuntime hits on
// first boot.

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

	// Windows-only env helpers matching FileSystem.cpp's own _dupenv_s/_putenv_s
	// usage; setting an empty value removes the variable for later tests.
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
} // namespace

TEST_CASE("InitializeDefaultMounts prepends a shipped project.pak shader layer over engine.pak")
{
	if (io::FileSystem::IsInitialized())
	{
		io::FileSystem::Shutdown();
	}

	// -- Build a fake engine.pak: shaders/common.spv + shaders/onlyengine.spv --
	const std::filesystem::path engineRoot = MakeTempDir("engine_root");
	WriteFile(engineRoot / "shaders" / "common.spv", "ENGINE_COMMON");
	WriteFile(engineRoot / "shaders" / "onlyengine.spv", "ENGINE_ONLY");
	const std::filesystem::path enginePak = std::filesystem::temp_directory_path() / "ae_fs_shader_overlay_engine.pak";
	std::filesystem::remove(enginePak);
	REQUIRE(assetpipeline::PackDirectory(engineRoot, enginePak, {}).ok);

	// -- Build a fake project.pak via the same shaderSpirvDir path the real
	// publisher uses: an (otherwise empty) project root packed with a compiled
	// -shader directory folded in under "shaders/" (PackWriter::AddDirectoryAs). --
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

	// Project overrides engine for a same-named shader.
	CHECK(io::FileSystem::Exists("shaders://common.spv"));
	const auto common = io::FileSystem::ReadFileText("shaders://common.spv");
	REQUIRE(common.has_value());
	CHECK(*common == "PROJECT_COMMON");

	// A project-only shader resolves.
	const auto projectOnly = io::FileSystem::ReadFileText("shaders://onlyproject.spv");
	REQUIRE(projectOnly.has_value());
	CHECK(*projectOnly == "PROJECT_ONLY");

	// An engine-only shader still falls through when the project layer misses.
	const auto engineOnly = io::FileSystem::ReadFileText("shaders://onlyengine.spv");
	REQUIRE(engineOnly.has_value());
	CHECK(*engineOnly == "ENGINE_ONLY");

	// Glob unions and de-dups: 3 distinct shader names, not 4.
	const auto glob = io::FileSystem::Glob("shaders://*.spv");
	REQUIRE(glob.has_value());
	CHECK(glob->size() == 3);

	io::FileSystem::Shutdown();
	ClearEnv("AETHER_ENGINE_PAK");
	ClearEnv("AETHER_PROJECT_PAK");
	ClearEnv("AETHER_ENGINE_MODE");
	ClearEnv("AETHER_PROJECT_MODE");
}
