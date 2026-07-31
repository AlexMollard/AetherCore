#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"

using namespace aether;

TEST_CASE("Apply overlays only the keys present in the document") {
    EngineSettings s;
    EngineSettingsIO::Apply("[graphics]\nfxaa = true\n", s);

    CHECK(s.graphics.fxaa == true);
    CHECK(s.graphics.vsync == true);
    CHECK(s.graphics.asyncCompute == true);
    CHECK(s.window.width == 2560);
}

TEST_CASE("Apply merges layers in order: later documents win") {
    EngineSettings s;
    EngineSettingsIO::Apply("[window]\nwidth = 1920\nheight = 1080\n", s);
    EngineSettingsIO::Apply("[window]\nwidth = 3840\n", s);

    CHECK(s.window.width == 3840);
    CHECK(s.window.height == 1080);
}

TEST_CASE("Sanitize resets invalid dimensions to defaults and clamps targetFps") {
    EngineSettings s;
    s.window.width = 0;
    s.window.height = -4;
    s.app.targetFps = -30.0f;

    EngineSettingsIO::Sanitize(s);

    CHECK(s.window.width == 2560);
    CHECK(s.window.height == 1440);
    CHECK(s.app.targetFps == doctest::Approx(0.0f));
}

TEST_CASE("Serialize round-trips through Apply for every field (no save/load drift)") {
    EngineSettings original;
    original.window.width = 2560;
    original.window.height = 1440;
    original.graphics.vsync = false;
    original.graphics.fxaa = true;
    original.graphics.asyncCompute = false;
    original.app.targetFps = 144.0f;
    original.app.startupScene = "level_two";
    original.app.autoplay = true;

    const std::string toml = EngineSettingsIO::Serialize(original);
    EngineSettings rebuilt;
    EngineSettingsIO::Apply(toml, rebuilt);

    CHECK(rebuilt.window.width == original.window.width);
    CHECK(rebuilt.window.height == original.window.height);
    CHECK(rebuilt.graphics.vsync == original.graphics.vsync);
    CHECK(rebuilt.graphics.fxaa == original.graphics.fxaa);
    CHECK(rebuilt.graphics.asyncCompute == original.graphics.asyncCompute);
    CHECK(rebuilt.app.targetFps == doctest::Approx(original.app.targetFps));
    CHECK(rebuilt.app.startupScene == original.app.startupScene);
    CHECK(rebuilt.app.autoplay == original.app.autoplay);
}

TEST_CASE("SerializeOverrides emits only keys that differ from the base") {
    EngineSettings base;
    EngineSettings current = base;
    current.graphics.fxaa = true;
    current.window.width = 3840;

    const std::string toml = EngineSettingsIO::SerializeOverrides(current, base);

    CHECK(toml.find("fxaa = true") != std::string::npos);
    CHECK(toml.find("width = 3840") != std::string::npos);
    // Unchanged keys must NOT be written, so they keep tracking shipped defaults.
    CHECK(toml.find("vsync") == std::string::npos);
    CHECK(toml.find("height") == std::string::npos);
    CHECK(toml.find("asyncCompute") == std::string::npos);
}

TEST_CASE("SerializeOverrides round-trips: applying the delta onto base reproduces current") {
    EngineSettings base;
    EngineSettings current = base;
    current.graphics.fxaa = true;
    current.graphics.vsync = false;
    current.window.width = 3440;
    current.window.height = 1440;
    current.app.targetFps = 120.0f;
    current.app.startupScene = "arena";
    current.app.autoplay = true;

    const std::string delta = EngineSettingsIO::SerializeOverrides(current, base);

    EngineSettings rebuilt = base;
    EngineSettingsIO::Apply(delta, rebuilt);

    CHECK(rebuilt.graphics.fxaa == current.graphics.fxaa);
    CHECK(rebuilt.graphics.vsync == current.graphics.vsync);
    CHECK(rebuilt.window.width == current.window.width);
    CHECK(rebuilt.window.height == current.window.height);
    CHECK(rebuilt.app.targetFps == doctest::Approx(current.app.targetFps));
    CHECK(rebuilt.app.autoplay == current.app.autoplay);
    // Every user-overridable field round-trips. The startup scene deliberately does not:
    // it is project data and never enters the per-user layer.
    CHECK(rebuilt.app.startupScene == base.app.startupScene);
}

TEST_CASE("SerializeOverrides with no changes writes no key lines") {
    EngineSettings base;
    EngineSettings current = base;

    const std::string toml = EngineSettingsIO::SerializeOverrides(current, base);

    CHECK(toml.find('[') == std::string::npos);
}

TEST_CASE("Full cascade: user value overrides shipped which overrides compiled default") {
    EngineSettings s;
    CHECK(s.graphics.vsync == true);

    EngineSettingsIO::Apply("[graphics]\nvsync = false\n", s);
    CHECK(s.graphics.vsync == false);

    EngineSettingsIO::Apply("[graphics]\nvsync = true\n", s);
    CHECK(s.graphics.vsync == true);
}

TEST_CASE("imguiViewports defaults on and round-trips through Apply") {
    EngineSettings s;
    CHECK(s.graphics.imguiViewports == true);

    EngineSettingsIO::Apply("[graphics]\nimguiViewports = false\n", s);
    CHECK(s.graphics.imguiViewports == false);

    const std::string toml = EngineSettingsIO::Serialize(s);
    EngineSettings rebuilt;
    EngineSettingsIO::Apply(toml, rebuilt);
    CHECK(rebuilt.graphics.imguiViewports == false);
}

TEST_CASE("uiScale defaults to 1 and Sanitize clamps to [0.5, 3.0]") {
    EngineSettings s;
    CHECK(s.graphics.uiScale == doctest::Approx(1.0f));

    EngineSettingsIO::Apply("[graphics]\nuiScale = 2.0\n", s);
    CHECK(s.graphics.uiScale == doctest::Approx(2.0f));

    s.graphics.uiScale = 10.0f;
    EngineSettingsIO::Sanitize(s);
    CHECK(s.graphics.uiScale == doctest::Approx(3.0f));

    s.graphics.uiScale = 0.1f;
    EngineSettingsIO::Sanitize(s);
    CHECK(s.graphics.uiScale == doctest::Approx(0.5f));
}

TEST_CASE("SerializeOverrides never writes the startup scene into the per-user file") {
    EngineSettings base;
    EngineSettings current = base;
    current.app.startupScene = "Title";
    current.graphics.fxaa = true;

    const std::string toml = EngineSettingsIO::SerializeOverrides(current, base);

    // The startup scene is project data: it belongs in ProjectSettings.toml so it reaches
    // published builds and every machine. A per-user copy would boot the right scene on
    // the machine that set it and an empty world everywhere else.
    CHECK(toml.find("startupScene") == std::string::npos);
    CHECK(toml.find("Title") == std::string::npos);
    CHECK(toml.find("fxaa = true") != std::string::npos);
}

TEST_CASE("A user-layer document cannot set the startup scene") {
    EngineSettings s;
    s.app.startupScene = "Title";

    EngineSettingsIO::Apply("[app]\nstartupScene = \"Stale\"\n[graphics]\nfxaa = true\n", s, SettingsScope::UserOverridable);

    // A stale machine-local override must never mask what the project says.
    CHECK(s.app.startupScene == "Title");
    CHECK(s.graphics.fxaa == true);
}

TEST_CASE("Project and shipped layers still set the startup scene") {
    EngineSettings s;
    EngineSettingsIO::Apply("[app]\nstartupscene = \"Arena\"\n", s);

    CHECK(s.app.startupScene == "Arena");
}

TEST_CASE("LoadLayered applies the project file as a layer and includes it in base") {
    const auto projectPath =
        std::filesystem::temp_directory_path() / "aethercore_projectsettings_xyztest.toml";
    REQUIRE(io::file_util::WriteText(projectPath, "[window]\nwidth = 1600\n[graphics]\nvsync = false\n").has_value());

    // Use shipped/user filenames that will NOT resolve on disk, so only compiled
    const auto loaded = EngineSettingsIO::LoadLayered(
        "nonexistent_shipped_xyztest.toml", projectPath, "nonexistent_user_xyztest.toml");

    CHECK(loaded.values.window.width == 1600);
    CHECK(loaded.base.window.width == 1600);
    CHECK(loaded.values.graphics.vsync == false);
    CHECK(loaded.values.window.height == 1440);

    std::error_code ec;
    std::filesystem::remove(projectPath, ec);
}

// MAILBOX does not block the producer, so an uncapped loop renders as fast as the hardware
// allows - measured at 3700 fps on a 2D game presenting 60. Flipping one setting must not
// put a user there, so Sanitize supplies a cap.
TEST_CASE("Requesting the low-latency present mode without a frame cap gets one") {
    EngineSettings settings;
    settings.graphics.vsync = true;
    settings.graphics.lowLatencyPresent = true;
    settings.app.targetFps = 0.0f;

    EngineSettingsIO::Sanitize(settings);

    CHECK(settings.app.targetFps == doctest::Approx(kUncappedMailboxFallbackFps));
}

TEST_CASE("An explicit frame cap survives the low-latency present mode") {
    EngineSettings settings;
    settings.graphics.vsync = true;
    settings.graphics.lowLatencyPresent = true;
    settings.app.targetFps = 240.0f;

    EngineSettingsIO::Sanitize(settings);

    CHECK(settings.app.targetFps == doctest::Approx(240.0f));
}

// FIFO already blocks on the vsync, so an uncapped loop there is self-limiting and must be
// left alone - a cap would be a behaviour change for every existing project.
TEST_CASE("The default present mode is left uncapped") {
    EngineSettings settings;
    settings.graphics.vsync = true;
    settings.graphics.lowLatencyPresent = false;
    settings.app.targetFps = 0.0f;

    EngineSettingsIO::Sanitize(settings);

    CHECK(settings.app.targetFps == doctest::Approx(0.0f));
}

TEST_CASE("Producer run-ahead is clamped to the resource slots that exist") {
    EngineSettings settings;

    settings.graphics.framesInFlight = 0;
    EngineSettingsIO::Sanitize(settings);
    CHECK(settings.graphics.framesInFlight == 1);

    settings.graphics.framesInFlight = 99;
    EngineSettingsIO::Sanitize(settings);
    CHECK(settings.graphics.framesInFlight == 3);
}
