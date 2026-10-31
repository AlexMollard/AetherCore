#include <doctest/doctest.h>

#include <chrono>
#include "io/FileUtil.hpp"

#include "utils/EngineSettings.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/SettingsService.hpp"

using namespace aether;

// These tests exercise the service's value/dirty plumbing and its null-safety

TEST_CASE("SettingsService exposes the values it was constructed with") {
    EngineSettings values;
    values.graphics.fxaa = true;
    values.window.width = 3840;
    EngineSettings base;

    ServiceContainer services;
    SettingsService service(values, base, base, services);

    CHECK(service.Get().graphics.fxaa == true);
    CHECK(service.Get().window.width == 3840);
    CHECK(service.Base().window.width == 2560);
}

TEST_CASE("ApplyField marks the settings dirty") {
    EngineSettings values;
    EngineSettings base;
    ServiceContainer services;
    SettingsService service(values, base, base, services);

    CHECK(service.IsDirty() == false);

    service.Values().graphics.fxaa = true;
    service.ApplyField("graphics.fxaa");
    CHECK(service.IsDirty() == true);
}

TEST_CASE("ApplyAll is a safe no-op when no subsystems are registered") {
    EngineSettings values;
    values.graphics.fxaa = true;
    values.app.targetFps = 120.0f;
    values.window.width = 3440;
    EngineSettings base;

    ServiceContainer services;

    SettingsService service(values, base, base, services);
    service.ApplyAll(); // must not crash / dereference a missing subsystem

    CHECK(service.Get().graphics.fxaa == true);
    CHECK(service.Get().app.targetFps == doctest::Approx(120.0f));
}

TEST_CASE("SettingsService retains dirty edits and errors until both settings files save")
{
	const auto directory = std::filesystem::temp_directory_path() / ("aether-settings-retry-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const auto blocker = directory / "blocked";
	REQUIRE(io::file_util::WriteText(blocker, "existing file"));
	EngineSettings values;
	EngineSettings base;
	ServiceContainer services;
	SettingsService service(values, base, base, services);
	service.MarkDirty();
	const auto userFile = directory / "user.toml";
	service.SetProjectFile(blocker / "project.toml");
	service.Save(userFile.string());
	CHECK(service.IsDirty());
	CHECK_FALSE(service.LastSaveError().empty());
	CHECK(io::file_util::Exists(userFile));
	service.SetProjectFile(directory / "project.toml");
	service.Save((blocker / "user.toml").string());
	CHECK(service.IsDirty());
	CHECK_FALSE(service.LastSaveError().empty());
	service.Save(userFile.string());
	CHECK_FALSE(service.IsDirty());
	CHECK(service.LastSaveError().empty());
	std::error_code error;
	for (const auto& path: {userFile, directory / "project.toml", blocker})
	{
		std::filesystem::remove(path, error);
		CHECK_FALSE(error);
	}
	std::filesystem::remove(directory, error);
	CHECK_FALSE(error);
}
