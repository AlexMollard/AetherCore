#include <doctest/doctest.h>

#include <filesystem>

#include "io/PlatformPaths.hpp"

#ifdef _WIN32
#include <windows.h>

TEST_CASE("Windows processes use UTF8 for native filesystem narrow conversions")
{
	REQUIRE(GetACP() == CP_UTF8);
	const std::filesystem::path path(L"C:\\Aether \u03a9\\\u65e5\u672c\u8a9e\\sc\u00e8ne \U0001f680.scene.toml");
	CHECK(path.string() == aether::io::PlatformPaths::PathToUtf8(path));
	CHECK(std::filesystem::u8path(path.string()) == path);
}
#endif
