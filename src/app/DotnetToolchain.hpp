#pragma once

#include <filesystem>
#include <string>

namespace aether::app
{
	// Where the `dotnet` CLI is on THIS machine.
	//
	// The editor shells out to it to build a project's C# scripts. That path used to be
	// baked in at compile time as an absolute path to the build machine's dotnet.exe,
	// which is a fact about the machine that compiled the engine and not about the one
	// running it. It happens to be right for a developer, and right by coincidence for a
	// Windows user with a default .NET install, and wrong for everyone else.
	//
	// Resolution order: DOTNET_ROOT (the variable .NET itself defines for this), then the
	// build-time path if it still exists, then PATH, then the platform's default install
	// location. Empty when .NET is not installed at all - which is a state the editor has
	// to be able to report, because the runtime alone is enough to RUN the editor while
	// only the SDK can BUILD scripts.
	//
	// Cheap to call: the answer is resolved once and cached for the process.
	[[nodiscard]] const std::filesystem::path& DotnetExecutable();

	// Whether a script build can be attempted at all.
	//
	// True requires an SDK, not merely a `dotnet` executable. Those are different things:
	// the CLI ships with the RUNTIME, so a machine that can run a game but not build one
	// passes any "is dotnet installed" test and then fails the build with a message from
	// MSBuild that reads, to the person holding it, as the script being broken.
	[[nodiscard]] bool HasDotnetToolchain();

	// Why HasDotnetToolchain() said no, phrased for the person who has to fix it - the two
	// causes have different fixes and one of them is easy to mistake for the other. Empty
	// when the toolchain is fine.
	[[nodiscard]] std::string DescribeMissingDotnetToolchain();

	// Whether the install containing `dotnetExe` can compile, i.e. has an sdk/<version>
	// directory beside the executable - the same thing the muxer looks for. Exposed because
	// it is the single distinction this whole file exists to make, and the one that was
	// missing: a runtime-only install has dotnet.exe and no sdk directory at all.
	[[nodiscard]] bool DotnetInstallHasSdk(const std::filesystem::path& dotnetExe);
} // namespace aether::app
