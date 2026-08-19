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
