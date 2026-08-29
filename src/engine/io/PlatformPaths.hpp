#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::io
{
	class PlatformPaths
	{
	public:
		PlatformPaths() = delete;

		[[nodiscard]] static std::filesystem::path GetExecutableDir();

		[[nodiscard]] static std::string GetExecutableName();

		[[nodiscard]] static std::filesystem::path ResolveToolExecutable(std::string_view envVar, std::string_view devHint, std::string_view fileName);

		// A file or directory the engine SHIPS - scene templates, prefab templates, the
		// managed SDK project. It sits in the source tree during development and inside
		// the executable's own bundle once packaged, and the same lookup has to answer
		// both without the caller knowing which kind of build it is running in.
		//
		// Same three-step policy as ResolveToolExecutable, for the same reason: an
		// environment override first so another checkout can win, then the build-injected
		// source path - absent from a packaged build, and during development the copy that
		// is actually being edited - then the staged copy beside the executable.
		//
		// Unlike ResolveToolExecutable this returns EMPTY when none of the three exist,
		// rather than a path that merely might. A missing template is something the caller
		// has to report ("no template to copy from"); handing back a plausible path only
		// moves the failure somewhere less legible.
		[[nodiscard]] static std::filesystem::path ResolveBundlePath(std::string_view envVar, std::string_view devHint, std::string_view bundleRelative);

		[[nodiscard]] static std::string ReadEnvironmentVariable(std::string_view name);

		[[nodiscard]] static std::filesystem::path GetUserConfigDir();

		// Where the user's own documents live, for content they author and keep - not the
		// config dir, which is machine state. Used as the default parent for new projects,
		// because the working directory is the engine build tree in a dev build and the
		// (often unwritable) install directory from a shortcut. Empty if undeterminable.
		[[nodiscard]] static std::filesystem::path GetUserDocumentsDir();

		static constexpr std::string_view kAppFolderName = "AetherCore";
	};
} // namespace aether::io
