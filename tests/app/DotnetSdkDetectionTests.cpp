// Telling a .NET RUNTIME apart from a .NET SDK.
//
// This exists because of a real report: someone without Visual Studio installed opened a
// project in the packaged editor, pressed Play, and was told the script failed to compile.
// Nothing was wrong with the script. The editor had checked whether a `dotnet` executable
// existed, found one, and shelled out to it - but `dotnet.exe` ships with the RUNTIME, and
// a runtime cannot build. MSBuild's own answer ("A compatible .NET SDK was not found") was
// surfaced as a compile error, which reads as the project being broken rather than as a
// missing tool.
//
// The distinction is a directory: an SDK install has sdk/<version> beside the executable
// and a runtime-only install has no sdk directory at all.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "DotnetToolchain.hpp"

using namespace aether::app;

namespace
{
	std::filesystem::path MakeFakeInstall(const std::string& tag)
	{
		const std::filesystem::path root = std::filesystem::temp_directory_path() / ("ae_dotnet_probe_" + tag);
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		std::ofstream(root / "dotnet.exe") << 'x';
		return root / "dotnet.exe";
	}
} // namespace

TEST_CASE("A dotnet install is only usable for building when it carries an SDK")
{
	namespace fs = std::filesystem;

	SUBCASE("a runtime-only install is rejected, which is the whole point")
	{
		// Exactly the shape dotnet-install.ps1 -Runtime dotnet produces: host/, shared/ and
		// dotnet.exe, and no sdk/ anywhere.
		const fs::path exe = MakeFakeInstall("runtime");
		fs::create_directories(exe.parent_path() / "host" / "fxr" / "10.0.11");
		fs::create_directories(exe.parent_path() / "shared" / "Microsoft.NETCore.App" / "10.0.11");
		CHECK_FALSE(DotnetInstallHasSdk(exe));
		fs::remove_all(exe.parent_path());
	}

	SUBCASE("an install with an SDK is accepted")
	{
		const fs::path exe = MakeFakeInstall("sdk");
		fs::create_directories(exe.parent_path() / "sdk" / "10.0.100");
		CHECK(DotnetInstallHasSdk(exe));
		fs::remove_all(exe.parent_path());
	}

	SUBCASE("an empty sdk directory is not an SDK")
	{
		// Uninstalling an SDK can leave the folder behind, and an empty one compiles nothing.
		const fs::path exe = MakeFakeInstall("emptysdk");
		fs::create_directories(exe.parent_path() / "sdk");
		CHECK_FALSE(DotnetInstallHasSdk(exe));
		fs::remove_all(exe.parent_path());
	}

	SUBCASE("a path that does not exist at all is not an SDK")
	{
		CHECK_FALSE(DotnetInstallHasSdk(fs::temp_directory_path() / "ae_dotnet_absent_zzz" / "dotnet.exe"));
	}
}

// The two causes have different fixes, and the message has to say which one applies:
// "install .NET" is wrong advice for someone who already has the runtime.
TEST_CASE("A missing toolchain is described, or not, according to what is actually present")
{
	const std::string described = DescribeMissingDotnetToolchain();
	if (HasDotnetToolchain())
	{
		CHECK(described.empty());
	}
	else
	{
		CHECK_FALSE(described.empty());
		// Whatever the cause, it must name the SDK as the thing to install - and say that
		// Visual Studio is not it, since that is the assumption the original report came in
		// with.
		CHECK(described.find("SDK") != std::string::npos);
		CHECK(described.find("Visual Studio") != std::string::npos);
	}
}
