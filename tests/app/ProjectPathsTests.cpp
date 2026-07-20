#include <doctest/doctest.h>

#include "project/ProjectPaths.hpp"

using aether::app::project::ComposeNewProjectRoot;
using aether::app::project::SanitizeProjectFolderName;

TEST_CASE("SanitizeProjectFolderName keeps a clean name and trims/collapses spaces")
{
    CHECK(SanitizeProjectFolderName("My Game") == "My Game");
    CHECK(SanitizeProjectFolderName("  My   Game  ") == "My Game");
}

TEST_CASE("SanitizeProjectFolderName strips illegal filename and control characters")
{
    CHECK(SanitizeProjectFolderName("a/b:c*?\"<>|d") == "abcd");
    CHECK(SanitizeProjectFolderName(std::string_view("x\ty", 3)) == "x y");
}

TEST_CASE("SanitizeProjectFolderName returns empty when nothing usable remains")
{
    CHECK(SanitizeProjectFolderName("").empty());
    CHECK(SanitizeProjectFolderName("///:::").empty());
    CHECK(SanitizeProjectFolderName("   ").empty());
}

TEST_CASE("ComposeNewProjectRoot joins parent and sanitized name")
{
    const auto root = ComposeNewProjectRoot("C:/projects", "My Game");
    CHECK(root.generic_string() == "C:/projects/My Game");
}

TEST_CASE("ComposeNewProjectRoot is empty when parent or sanitized name is empty")
{
    CHECK(ComposeNewProjectRoot("", "X").empty());
    CHECK(ComposeNewProjectRoot("C:/projects", "///").empty());
}
