#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "utils/LayoutPresetStore.hpp"

using namespace aether;

TEST_CASE("LayoutPresetStore serialize/deserialize round-trips the ini verbatim") {
    LayoutPreset p;
    p.name = "My Layout";
    p.visibility = {{"Scene", true}, {"Console", false}, {"Day / Night", true}};
    p.imguiIni =
        "[Window][Debug]\nPos=60,60\nSize=400,300\n\n"
        "[Docking][Data]\nDockNode ID=0x01 Pos=0,0 Size=1280,720 Split=X\n";

    const std::string text = LayoutPresetStore::Serialize(p);
    const auto back = LayoutPresetStore::Deserialize(text);

    REQUIRE(back.has_value());
    CHECK(back->name == "My Layout");
    REQUIRE(back->visibility.size() == 3);
    CHECK(back->visibility[0] == std::pair<std::string, bool>{"Scene", true});
    CHECK(back->visibility[1] == std::pair<std::string, bool>{"Console", false});
    CHECK(back->visibility[2] == std::pair<std::string, bool>{"Day / Night", true});
    CHECK(back->imguiIni == p.imguiIni);
}

TEST_CASE("LayoutPresetStore deserialize rejects text with no [imgui] marker") {
    CHECK_FALSE(LayoutPresetStore::Deserialize("name = X\n[visibility]\nScene = 1\n").has_value());
}

TEST_CASE("LayoutPresetStore deserialize accepts an empty ini tail") {
    const auto back = LayoutPresetStore::Deserialize("name = Bare\n[visibility]\n[imgui]\n");
    REQUIRE(back.has_value());
    CHECK(back->name == "Bare");
    CHECK(back->visibility.empty());
    CHECK(back->imguiIni.empty());
}

TEST_CASE("LayoutPresetStore SlugFor produces a filesystem-safe stem") {
    CHECK(LayoutPresetStore::SlugFor("Day / Night") == "Day__Night");
    CHECK(LayoutPresetStore::SlugFor("Alpha_1-2") == "Alpha_1-2");
    CHECK(LayoutPresetStore::SlugFor("***") == "layout");             // never empty
}

TEST_CASE("LayoutPresetStore Save/LoadAll/Remove on disk") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aethercore_layout_preset_test";
    std::error_code ec;
    fs::remove_all(dir, ec);

    LayoutPreset a;
    a.name = "Alpha";
    a.imguiIni = "[Window][X]\nPos=1,2\n";
    a.visibility = {{"Scene", true}};
    LayoutPreset b;
    b.name = "Beta";
    b.imguiIni = "[Window][Y]\nPos=3,4\n";
    b.visibility = {{"Console", false}};

    REQUIRE(LayoutPresetStore::Save(a, dir));
    REQUIRE(LayoutPresetStore::Save(b, dir));

    auto all = LayoutPresetStore::LoadAll(dir);
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "Alpha");
    CHECK(all[1].name == "Beta");
    CHECK(all[0].imguiIni == a.imguiIni);

    CHECK(LayoutPresetStore::Remove("Alpha", dir));
    all = LayoutPresetStore::LoadAll(dir);
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "Beta");

    fs::remove_all(dir, ec);
}
