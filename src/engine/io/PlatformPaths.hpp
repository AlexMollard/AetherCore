#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::io
{
	// Resolves OS-provided, build-system-independent locations. These are used to
	// find the running executable (for locating shipped, read-only data beside it)
	// and the per-user writable directory (for settings a player changes in game).
	//
	// Nothing here depends on CMake, the working directory, or the VFS mount table,
	// so it behaves identically in a dev build tree and a shipped install.
	class PlatformPaths
	{
	public:
		PlatformPaths() = delete;

		// Absolute directory containing the running executable. Returns the current
		// working directory as a last-resort fallback if the OS query fails.
		[[nodiscard]] static std::filesystem::path GetExecutableDir();

		// Filename of the running executable, without its extension (e.g. "App" or
		// "AetherGame"). Used to key per-user cache files so distinct binaries that
		// share the same LocalAppData/AetherCore folder (editor vs. shipped game,
		// or multiple published games built from this engine) don't stomp each
		// other's caches. Returns an empty string if the OS query fails.
		[[nodiscard]] static std::string GetExecutableName();

		// Per-user writable directory for this application's settings, created on
		// demand:
		//   Windows: %LOCALAPPDATA%\AetherCore
		//   Linux:   $XDG_CONFIG_HOME/AetherCore  (falls back to ~/.config/AetherCore)
		// Machine-specific settings (resolution, GPU toggles) belong here rather
		// than a roaming profile. Returns an empty path only if no home/appdata
		// location can be determined at all.
		[[nodiscard]] static std::filesystem::path GetUserConfigDir();

		// Folder name appended under the OS user-data root. Central so a rename is a
		// one-line change.
		static constexpr std::string_view kAppFolderName = "AetherCore";
	};
} // namespace aether::io
