#include "DotnetToolchain.hpp"

#include <string>
#include <string_view>
#include <vector>
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

		// Whether this dotnet can BUILD, not merely run.
		//
		// The distinction is the whole point: `dotnet.exe` ships with the RUNTIME, so its
		// presence says nothing about whether a compiler is installed. A runtime-only
		// machine passes any "is there a dotnet" test and then fails the build with "A
		// compatible .NET SDK was not found", which reaches the user as a script that would
		// not compile for no stated reason. This is the check that was missing.
		//
		// Looked for the same way the muxer looks for it - an sdk/<version> directory beside
		// the executable - rather than by running `dotnet --list-sdks`, which is authoritative
		// but costs a process launch on a path that runs during startup.
		struct Resolved
		{
			std::filesystem::path path;
			bool hasSdk = false;
		};

		Resolved ResolveDotnetOnce()
		{
			std::vector<std::filesystem::path> candidates;
			if (std::filesystem::path fromRoot = FromDotnetRoot(); !fromRoot.empty())
			{
				candidates.push_back(std::move(fromRoot));
			}
			if (!kDotnetDevHint.empty())
			{
				if (std::filesystem::path hinted{std::string(kDotnetDevHint)}; IsExecutableFile(hinted))
				{
					candidates.push_back(std::move(hinted));
				}
			}
			if (std::filesystem::path fromPath = FromPath(); !fromPath.empty())
			{
				candidates.push_back(std::move(fromPath));
			}
			if (std::filesystem::path fromInstall = FromDefaultInstall(); !fromInstall.empty())
			{
				candidates.push_back(std::move(fromInstall));
			}

			// An install that can build wins over one that cannot, wherever it was found:
			// a runtime-only dotnet earlier on PATH must not hide a real SDK in the default
			// location. Failing that, keep the first dotnet found so the caller can say
			// "runtime but no SDK" rather than "no .NET at all", which are different
			// problems with different fixes.
			for (const std::filesystem::path& candidate: candidates)
			{
				if (DotnetInstallHasSdk(candidate))
				{
					return {candidate, true};
				}
			}
			return candidates.empty() ? Resolved{} : Resolved{candidates.front(), false};
		}

		const Resolved& ResolvedDotnet()
		{
			static const Resolved resolved = []
			{
				Resolved r = ResolveDotnetOnce();
				if (r.path.empty())
				{
					AE_WARN(LogCategory::App, "No .NET found: C# scripts cannot be compiled. Install the .NET SDK from dotnet.microsoft.com/download.");
				}
				else if (!r.hasSdk)
				{
					AE_WARN(LogCategory::App,
					        "Found the .NET runtime at '{}' but no SDK beside it. Running a game needs only the runtime; COMPILING its C# scripts needs the .NET SDK - install it from dotnet.microsoft.com/download.",
					        r.path.string());
				}
				else
				{
					AE_INFO(LogCategory::App, "dotnet SDK: {}", r.path.string());
				}
				return r;
			}();
			return resolved;
		}
	} // namespace

	bool DotnetInstallHasSdk(const std::filesystem::path& dotnetExe)
	{
		std::error_code ec;
		const std::filesystem::path sdkDir = dotnetExe.parent_path() / "sdk";
		if (!std::filesystem::is_directory(sdkDir, ec))
		{
			return false;
		}
		// At least one version directory: an SDK that was uninstalled can leave the folder
		// behind, and an empty one compiles nothing.
		for (const auto& entry: std::filesystem::directory_iterator(sdkDir, ec))
		{
			if (entry.is_directory(ec))
			{
				return true;
			}
		}
		return false;
	}

	const std::filesystem::path& DotnetExecutable()
	{
		return ResolvedDotnet().path;
	}

	bool HasDotnetToolchain()
	{
		return ResolvedDotnet().hasSdk;
	}

	std::string DescribeMissingDotnetToolchain()
	{
		const Resolved& r = ResolvedDotnet();
		if (r.path.empty())
		{
			return "No .NET installation was found, so this project's C# scripts cannot be compiled. "
			       "Install the .NET SDK from https://dotnet.microsoft.com/download and reopen the project. "
			       "Visual Studio is not required - the SDK on its own is enough.";
		}
		if (!r.hasSdk)
		{
			return "Found the .NET runtime at '" + r.path.string() + "', but only the runtime - there is no SDK beside it. "
			       "The runtime can RUN a game; compiling its C# scripts needs the .NET SDK. "
			       "Install the SDK from https://dotnet.microsoft.com/download and reopen the project. "
			       "Visual Studio is not required.";
		}
		return {};
	}
} // namespace aether::app
