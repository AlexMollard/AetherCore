#include "DotnetToolchain.hpp"

#include <string>
#include <string_view>
#include <system_error>

#include "io/PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
	namespace
	{
#ifdef _WIN32
		constexpr std::string_view kDotnetFileName = "dotnet.exe";
		constexpr char kPathSeparator = ';';
#else
		constexpr std::string_view kDotnetFileName = "dotnet";
		constexpr char kPathSeparator = ':';
#endif

#ifdef AETHER_DOTNET_EXE
		constexpr std::string_view kDotnetDevHint = AETHER_DOTNET_EXE;
#else
		constexpr std::string_view kDotnetDevHint = {};
#endif

		bool IsExecutableFile(const std::filesystem::path& path)
		{
			std::error_code ec;
			return !path.empty() && std::filesystem::is_regular_file(path, ec);
		}

		// DOTNET_ROOT is a directory; the CLI sits directly inside it.
		std::filesystem::path FromDotnetRoot()
		{
			const std::string root = io::PlatformPaths::ReadEnvironmentVariable("DOTNET_ROOT");
			if (root.empty())
			{
				return {};
			}
			std::filesystem::path candidate = std::filesystem::path(root) / kDotnetFileName;
			return IsExecutableFile(candidate) ? candidate : std::filesystem::path{};
		}

		std::filesystem::path FromPath()
		{
			const std::string pathVar = io::PlatformPaths::ReadEnvironmentVariable("PATH");
			std::size_t begin = 0;
			while (begin <= pathVar.size())
			{
				const std::size_t end = pathVar.find(kPathSeparator, begin);
				const std::string_view entry = std::string_view(pathVar).substr(begin, end == std::string::npos ? std::string::npos : end - begin);
				if (!entry.empty())
				{
					if (std::filesystem::path candidate = std::filesystem::path(std::string(entry)) / kDotnetFileName; IsExecutableFile(candidate))
					{
						return candidate;
					}
				}
				if (end == std::string::npos)
				{
					break;
				}
				begin = end + 1;
			}
			return {};
		}

		// The platform's default install location, for the common case of .NET installed
		// by its own installer into a shell that has not been restarted, so PATH in this
		// process predates it.
		std::filesystem::path FromDefaultInstall()
		{
#ifdef _WIN32
			for (const std::string_view variable: {"ProgramFiles", "ProgramW6432"})
			{
				const std::string root = io::PlatformPaths::ReadEnvironmentVariable(variable);
				if (root.empty())
				{
					continue;
				}
				if (std::filesystem::path candidate = std::filesystem::path(root) / "dotnet" / kDotnetFileName; IsExecutableFile(candidate))
				{
					return candidate;
				}
			}
#else
			for (const std::string_view root: {"/usr/share/dotnet", "/usr/lib/dotnet", "/usr/local/share/dotnet"})
			{
				if (std::filesystem::path candidate = std::filesystem::path(std::string(root)) / kDotnetFileName; IsExecutableFile(candidate))
				{
					return candidate;
				}
			}
#endif
			return {};
		}

		std::filesystem::path ResolveDotnetOnce()
		{
			if (std::filesystem::path fromRoot = FromDotnetRoot(); !fromRoot.empty())
			{
				return fromRoot;
			}
			if (!kDotnetDevHint.empty())
			{
				if (std::filesystem::path hinted{std::string(kDotnetDevHint)}; IsExecutableFile(hinted))
				{
					return hinted;
				}
			}
			if (std::filesystem::path fromPath = FromPath(); !fromPath.empty())
			{
				return fromPath;
			}
			return FromDefaultInstall();
		}
	} // namespace

	const std::filesystem::path& DotnetExecutable()
	{
		static const std::filesystem::path resolved = []
		{
			std::filesystem::path path = ResolveDotnetOnce();
			if (path.empty())
			{
				AE_WARN(LogCategory::App, "No .NET SDK found: C# scripts cannot be compiled. Install the .NET SDK, or set DOTNET_ROOT.");
			}
			else
			{
				AE_INFO(LogCategory::App, "dotnet CLI: {}", path.string());
			}
			return path;
		}();
		return resolved;
	}

	bool HasDotnetToolchain()
	{
		return !DotnetExecutable().empty();
	}
} // namespace aether::app
