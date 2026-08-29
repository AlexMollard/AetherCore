#pragma once

#include <filesystem>

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

	// Whether a script build can be attempted at all. False means "no .NET SDK here",
	// which callers should surface to the user rather than treat as a build failure.
	[[nodiscard]] bool HasDotnetToolchain();
} // namespace aether::app
