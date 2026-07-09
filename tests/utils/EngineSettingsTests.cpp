#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"

using namespace aether;

TEST_CASE("Apply overlays only the keys present in the document") {
    EngineSettings s; // compiled-in defaults
    EngineSettingsIO::Apply("[graphics]\nfxaa = true\n", s);

    CHECK(s.graphics.fxaa == true);          // changed
    CHECK(s.graphics.vsync == true);         // untouched default
    CHECK(s.graphics.asyncCompute == true);  // untouched default
    CHECK(s.window.width == 1280);           // untouched default
}

TEST_CASE("Apply merges layers in order: later documents win") {
    EngineSettings s;
    EngineSettingsIO::Apply("[window]\nwidth = 1920\nheight = 1080\n", s); // shipped
    EngineSettingsIO::Apply("[window]\nwidth = 2560\n", s);                // user override

    CHECK(s.window.width == 2560);   // user layer wins
    CHECK(s.window.height == 1080);  // still from shipped layer
}

TEST_CASE("Sanitize resets invalid dimensions to defaults and clamps targetFps") {
    EngineSettings s;
    s.window.width = 0;
    s.window.height = -4;
    s.app.targetFps = -30.0f;

    EngineSettingsIO::Sanitize(s);

    CHECK(s.window.width == 1280);   // reset to compiled default
    CHECK(s.window.height == 720);   // reset to compiled default
    CHECK(s.app.targetFps == doctest::Approx(0.0f)); // clamped
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
    EngineSettings rebuilt; // defaults
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
    EngineSettings base; // defaults
    EngineSettings current = base;
    current.graphics.fxaa = true;
    current.window.width = 2560;

    const std::string toml = EngineSettingsIO::SerializeOverrides(current, base);

    CHECK(toml.find("fxaa = true") != std::string::npos);
    CHECK(toml.find("width = 2560") != std::string::npos);
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
    CHECK(rebuilt.app.startupScene == current.app.startupScene);
    CHECK(rebuilt.app.autoplay == current.app.autoplay);
}

TEST_CASE("SerializeOverrides with no changes writes no key lines") {
    EngineSettings base;
    EngineSettings current = base;

    const std::string toml = EngineSettingsIO::SerializeOverrides(current, base);

    CHECK(toml.find('[') == std::string::npos); // no section headers => nothing changed
}

TEST_CASE("Full cascade: user value overrides shipped which overrides compiled default") {
    // Layer 1: compiled default vsync = true.
    EngineSettings s;
    CHECK(s.graphics.vsync == true);

    // Layer 2: shipped flips it off.
    EngineSettingsIO::Apply("[graphics]\nvsync = false\n", s);
    CHECK(s.graphics.vsync == false);

    // Layer 3: user turns it back on.
    EngineSettingsIO::Apply("[graphics]\nvsync = true\n", s);
    CHECK(s.graphics.vsync == true);
}

TEST_CASE("imguiViewports defaults on and round-trips through Apply") {
    EngineSettings s; // compiled-in defaults
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
    CHECK(s.graphics.uiScale == doctest::Approx(3.0f)); // clamped high

    s.graphics.uiScale = 0.1f;
    EngineSettingsIO::Sanitize(s);
    CHECK(s.graphics.uiScale == doctest::Approx(0.5f)); // clamped low
}

TEST_CASE("LoadLayered applies the project file as a layer and includes it in base") {
    // Write a temp ProjectSettings.toml and load it through the real LoadLayered.
    const auto projectPath =
        std::filesystem::temp_directory_path() / "aethercore_projectsettings_xyztest.toml";
    REQUIRE(io::file_util::WriteText(projectPath, "[window]\nwidth = 1600\n[graphics]\nvsync = false\n").has_value());

    // Use shipped/user filenames that will NOT resolve on disk, so only compiled
    // defaults + the temp project file participate (independent of the real
    // user-config dir / build tree).
    const auto loaded = EngineSettingsIO::LoadLayered(
        "nonexistent_shipped_xyztest.toml", projectPath, "nonexistent_user_xyztest.toml");

    CHECK(loaded.values.window.width == 1600);    // project layer applied by LoadLayered
    CHECK(loaded.base.window.width == 1600);      // base INCLUDES the project layer (regression guard)
    CHECK(loaded.values.graphics.vsync == false); // project override took effect
    CHECK(loaded.values.window.height == 720);    // compiled default not in project file is untouched

    std::error_code ec;
    std::filesystem::remove(projectPath, ec);
}
