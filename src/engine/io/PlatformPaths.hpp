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

		static constexpr std::string_view kAppFolderName = "AetherCore";
	};
} // namespace aether::io
