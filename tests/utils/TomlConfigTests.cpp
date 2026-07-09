#include <doctest/doctest.h>

#include <filesystem>
#include <sstream>
#include <system_error>

#include "io/FileUtil.hpp"
#include "utils/TomlConfig.hpp"

using namespace aether;

// A section-less (top-level) key must survive a save/reload round-trip. The
// serializer groups keys into [section] blocks by splitting the map key on its
// first '.', but it walks the keys in sorted order. A section-less key that
// sorts *after* a section was emitted with no header directly below that
// section's block, so on reload TOML re-parented it under the preceding table.
TEST_CASE("TomlConfig: top-level key round-trips when it sorts after a section")
{
    TomlConfig cfg;
    cfg.Set("launcher.name", std::string_view("Testing")); // section 'launcher'
    cfg.Set("rendergraphautoselecthotpass", false);        // sorts after 'launcher.'

    std::ostringstream saved;
    cfg.Save(saved);

    TomlConfig reloaded;
    reloaded.Load(saved.str());

    CHECK(reloaded.Has("rendergraphautoselecthotpass"));
    CHECK_FALSE(reloaded.Has("launcher.rendergraphautoselecthotpass"));
    CHECK(reloaded.GetBool("rendergraphautoselecthotpass", true) == false);
}

// Regression for the reported crash: a bare key reparented into a section on
// load, then re-Set at top level, previously produced a file with the same key
// emitted twice under one table -> "cannot redefine existing boolean" on the
// next parse -> the whole config was rejected and every setting was lost.
TEST_CASE("TomlConfig: repeated save/reload does not corrupt into duplicate keys")
{
    TomlConfig cfg;
    cfg.Set("launcher.name", std::string_view("Testing"));
    cfg.Set("rendergraphautoselecthotpass", false);

    std::ostringstream first;
    cfg.Save(first);

    TomlConfig second;
    second.Load(first.str());
    second.Set("rendergraphautoselecthotpass", false); // panel writes flat key again

    std::ostringstream secondOut;
    second.Save(secondOut);

    // Re-parsing must succeed: a duplicate-key parse error drops every value.
    TomlConfig third;
    third.Load(secondOut.str());
    CHECK(third.Has("launcher.name"));
    CHECK(third.Has("rendergraphautoselecthotpass"));
}

TEST_CASE("TomlConfig: LoadFromPath/SaveToPath round-trips an absolute path") {
    const auto path = std::filesystem::temp_directory_path() / "aether_editorstate_test.toml";
    std::error_code ec; std::filesystem::remove(path, ec);

    TomlConfig out;
    out.Set("window.viewport", true);
    REQUIRE(out.SaveToPath(path, "Editor state"));

    TomlConfig in;
    REQUIRE(in.LoadFromPath(path));
    CHECK(in.GetBool("window.viewport", false) == true);

    std::filesystem::remove(path, ec);
}
