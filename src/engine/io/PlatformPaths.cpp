#include "PlatformPaths.hpp"

#include <cstdlib>
#include <string>
#include <system_error>

#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>

#	include <shlobj.h>

// (lld-link), so no CMakeLists change is required.
#	pragma comment(lib, "Shell32.lib")
#	pragma comment(lib, "Ole32.lib")
#elif defined(__linux__)
#	include <limits.h>
#	include <unistd.h>
#endif

namespace aether::io
{
	namespace
	{
		std::filesystem::path s_explicitProjectDirectory;

		std::string EnvironmentString(std::string_view name)
		{
#ifdef _MSC_VER
			char* value = nullptr;
			std::size_t size = 0;
			const std::string nameString(name);
			if (_dupenv_s(&value, &size, nameString.c_str()) != 0 || value == nullptr)
			{
				return {};
			}
			std::string result(value);
			std::free(value);
			return result;
#else
			const std::string nameString(name);
			if (const char* value = std::getenv(nameString.c_str()); value != nullptr)
			{
				return value;
			}
			return {};
#endif
		}

		std::filesystem::path EnsureDirectory(std::filesystem::path dir)
		{
			if (dir.empty())
			{
				return dir;
			}
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			if (ec)
			{
				AE_WARN(LogCategory::FileSystem, "Failed to create user directory '{}': {}", dir.string(), ec.message());
			}
			return dir;
		}

		std::filesystem::path ResolveIsolatedBundlePath(std::string_view relative)
		{
			const auto path = std::filesystem::u8path(relative);
			if (path.empty() || path.is_absolute() || path.has_root_path())
			{
				return {};
			}
			std::error_code ec;
			const auto root = std::filesystem::canonical(PlatformPaths::GetExecutableDir(), ec);
			if (ec)
			{
				return {};
			}
			const auto bundled = std::filesystem::canonical(root / path, ec);
			if (ec)
			{
				return {};
			}
			const auto descendant = bundled.lexically_relative(root);
			if (descendant.empty() || descendant.is_absolute())
			{
				return {};
			}
			for (const auto& part : descendant)
			{
				if (part == "..")
				{
					return {};
				}
			}
			return bundled;
		}

		std::filesystem::path ResolveExecutablePath()
		{
#if defined(_WIN32)
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length == 0)
				{
					break;
				}
				if (length < buffer.size())
				{
					buffer.resize(length);
					return std::filesystem::path(buffer);
				}
				buffer.resize(buffer.size() * 2);
			}
#elif defined(__linux__)
			std::string buffer(PATH_MAX, '\0');
			const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
			if (length > 0)
			{
				buffer.resize(static_cast<std::size_t>(length));
				return std::filesystem::path(buffer);
			}
#endif
			return {};
		}
	} // namespace

	std::string PlatformPaths::ReadEnvironmentVariable(std::string_view name)
	{
		return EnvironmentString(name);
	}

	bool PlatformPaths::IsIsolatedProduct()
	{
		return IsIsolatedProduct(GetExecutableDir(), EnvironmentString("AETHER_ISOLATED_PRODUCT"));
	}

	bool PlatformPaths::IsIsolatedProduct(const std::filesystem::path& executableDir, std::string_view forceFlag)
	{
		if (forceFlag == "1")
		{
			return true;
		}
		if (executableDir.empty())
		{
			return false;
		}
		std::error_code ec;
		return std::filesystem::is_regular_file(executableDir / "BUILD_PROVENANCE.json", ec)
		    || std::filesystem::is_regular_file(executableDir / "aether-product", ec);
	}

	void PlatformPaths::SetExplicitProjectDirectory(const std::filesystem::path& directory)
	{
		std::error_code ec;
		s_explicitProjectDirectory = directory.empty() ? std::filesystem::path{} : std::filesystem::absolute(directory, ec).lexically_normal();
		if (ec)
		{
			s_explicitProjectDirectory.clear();
		}
	}

	std::filesystem::path PlatformPaths::GetExplicitProjectDirectory()
	{
		return s_explicitProjectDirectory;
	}

	std::filesystem::path PlatformPaths::ReadEnvironmentPath(std::string_view name)
	{
#ifdef _WIN32
		const std::wstring wideName(name.begin(), name.end());
		const DWORD size = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
		if (size == 0)
		{
			return {};
		}
		std::wstring value(size, L'\0');
		const DWORD copied = GetEnvironmentVariableW(wideName.c_str(), value.data(), size);
		if (copied == 0 || copied >= size)
		{
			return {};
		}
		value.resize(copied);
		return std::filesystem::path(value);
#else
		return std::filesystem::path(EnvironmentString(name));
#endif
	}

	std::string PlatformPaths::PathToUtf8(const std::filesystem::path& path)
	{
		const auto bytes = path.u8string();
		return std::string(bytes.begin(), bytes.end());
	}

	std::filesystem::path PlatformPaths::GetExecutableDir()
	{
		if (const std::filesystem::path exePath = ResolveExecutablePath(); !exePath.empty())
		{
			return exePath.parent_path();
		}

		std::error_code ec;
		auto cwd = std::filesystem::current_path(ec);
		return ec ? std::filesystem::path{} : cwd;
	}

	std::string PlatformPaths::GetExecutableName()
	{
		const std::filesystem::path exePath = ResolveExecutablePath();
		return exePath.empty() ? std::string{} : exePath.stem().string();
	}

	bool PlatformPaths::IsAgentSession()
	{
		static const bool agent = []
		{
			if (const char* env = std::getenv("AETHER_AGENT_SESSION"); env != nullptr && *env != '\0' && std::string_view(env) != "0")
			{
				return true;
			}
			std::error_code ec;
			return std::filesystem::exists(GetExecutableDir() / "agent-session", ec);
		}();
		return agent;
	}

	std::string PlatformPaths::AgentSessionName()
	{
		static const std::string name = []
		{
			if (!IsAgentSession())
			{
				return std::string{};
			}
			if (const char* env = std::getenv("AETHER_AGENT_NAME"); env != nullptr && *env != '\0')
			{
				return std::string{env};
			}
			constexpr std::string_view kSnapPrefix = "editor-snap-";
			for (std::filesystem::path dir = GetExecutableDir(); !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path())
			{
				const std::string leaf = dir.filename().string();
				if (leaf.starts_with(kSnapPrefix) && leaf.size() > kSnapPrefix.size())
				{
					return leaf.substr(kSnapPrefix.size());
				}
			}
			return std::string{};
		}();
		return name;
	}

	std::filesystem::path PlatformPaths::FindExecutable(std::string_view name)
	{
		const auto paths = ReadEnvironmentVariable("PATH");
#ifdef _WIN32
		constexpr char separator = ';';
		const auto filename = std::string(name) + ".exe";
#else
		constexpr char separator = ':';
		const std::string filename(name);
#endif
		std::size_t begin = 0;
		while (begin < paths.size())
		{
			const auto end = paths.find(separator, begin);
			auto entry = paths.substr(begin, end == std::string::npos ? end : end - begin);
			if (entry.size() > 1 && entry.front() == '"' && entry.back() == '"')
			{
				entry = entry.substr(1, entry.size() - 2);
			}
			const auto candidate = std::filesystem::u8path(entry) / filename;
			if (!entry.empty() && std::filesystem::is_regular_file(candidate))
			{
				return candidate;
			}
			if (end == std::string::npos)
			{
				break;
			}
			begin = end + 1;
		}
		return {};
	}

	std::filesystem::path PlatformPaths::ResolveToolExecutable(std::string_view envVar, std::string_view devHint, std::string_view fileName)
	{
		std::error_code ec;
		if (IsIsolatedProduct())
		{
			const auto bundled = ResolveIsolatedBundlePath(fileName);
			return std::filesystem::is_regular_file(bundled, ec) ? bundled : std::filesystem::path{};
		}
		if (!envVar.empty())
		{
			if (const std::string envValue = EnvironmentString(envVar); !envValue.empty() && std::filesystem::exists(envValue, ec))
			{
				return std::filesystem::path(envValue);
			}
		}
		if (!devHint.empty())
		{
			if (std::filesystem::path hinted{std::string(devHint)}; std::filesystem::exists(hinted, ec))
			{
				return hinted;
			}
		}
		return GetExecutableDir() / std::filesystem::path(std::string(fileName));
	}

	std::filesystem::path PlatformPaths::ResolveBundlePath(std::string_view envVar, std::string_view devHint, std::string_view bundleRelative)
	{
		std::error_code ec;
		if (IsIsolatedProduct())
		{
			return ResolveIsolatedBundlePath(bundleRelative);
		}
		if (!envVar.empty())
		{
			if (const std::string envValue = EnvironmentString(envVar); !envValue.empty() && std::filesystem::exists(envValue, ec))
			{
				return std::filesystem::path(envValue);
			}
		}
		if (!devHint.empty())
		{
			if (std::filesystem::path hinted{std::string(devHint)}; std::filesystem::exists(hinted, ec))
			{
				return hinted;
			}
		}
		if (!bundleRelative.empty())
		{
			if (std::filesystem::path staged = GetExecutableDir() / std::filesystem::path(std::string(bundleRelative)); std::filesystem::exists(staged, ec))
			{
				return staged;
			}
		}
		return {};
	}

	std::filesystem::path PlatformPaths::ResolveUserUiLayoutPath(const std::filesystem::path& userConfigDir, const std::string_view executableName)
	{
		const auto name = std::filesystem::u8path(executableName);
		if (userConfigDir.empty() || !userConfigDir.is_absolute() || name.empty() || name.has_parent_path() || name.has_root_path() || name == "." || name == "..")
		{
			return {};
		}
		return userConfigDir / "ui" / (name.native() + std::filesystem::path(".ini").native());
	}

	std::filesystem::path PlatformPaths::GetUserConfigDir()
	{
		if (const auto overridePath = ReadEnvironmentPath("AETHER_USER_DATA_DIR"); !overridePath.empty())
		{
			if (!overridePath.is_absolute())
			{
				AE_ERROR(LogCategory::FileSystem, "AETHER_USER_DATA_DIR must be absolute; user settings disabled.");
				return {};
			}
			std::error_code ec;
			std::filesystem::create_directories(overridePath, ec);
			if (ec || !std::filesystem::is_directory(overridePath, ec))
			{
				AE_ERROR(LogCategory::FileSystem, "Cannot use AETHER_USER_DATA_DIR '{}'; user settings disabled.", PathToUtf8(overridePath));
				return {};
			}
			return overridePath;
		}
		std::filesystem::path root;

#if defined(_WIN32)
		PWSTR rawPath = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath != nullptr)
		{
			root = std::filesystem::path(rawPath);
		}
		if (rawPath != nullptr)
		{
			CoTaskMemFree(rawPath);
		}
		if (root.empty())
		{
			if (const std::string env = EnvironmentString("LOCALAPPDATA"); !env.empty())
			{
				root = env;
			}
		}
#else
		if (const std::string xdg = EnvironmentString("XDG_CONFIG_HOME"); !xdg.empty())
		{
			root = xdg;
		}
		else if (const std::string home = EnvironmentString("HOME"); !home.empty())
		{
			root = std::filesystem::path(home) / ".config";
		}
#endif

		if (root.empty())
		{
			AE_WARN(LogCategory::FileSystem, "Could not determine a per-user config directory; settings will not persist.");
			return {};
		}

		return EnsureDirectory(root / kAppFolderName);
	}

	std::filesystem::path PlatformPaths::GetUserDocumentsDir()
	{
		std::filesystem::path root;

#if defined(_WIN32)
		PWSTR rawPath = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath != nullptr)
		{
			root = std::filesystem::path(rawPath);
		}
		if (rawPath != nullptr)
		{
			CoTaskMemFree(rawPath);
		}
		if (root.empty())
		{
			// Redirected or roaming Documents can fail the known-folder lookup; the profile
			// root is still somewhere the user can write.
			if (const std::string env = EnvironmentString("USERPROFILE"); !env.empty())
			{
				root = std::filesystem::path(env) / "Documents";
			}
		}
#else
		if (const std::string xdg = EnvironmentString("XDG_DOCUMENTS_DIR"); !xdg.empty())
		{
			root = xdg;
		}
		else if (const std::string home = EnvironmentString("HOME"); !home.empty())
		{
			// ~/Documents is not guaranteed to exist on a headless or minimal install;
			// fall back to the home directory rather than inventing a tree there.
			const std::filesystem::path documents = std::filesystem::path(home) / "Documents";
			std::error_code ec;
			root = std::filesystem::is_directory(documents, ec) ? documents : std::filesystem::path(home);
		}
#endif

		if (root.empty())
		{
			return {};
		}

		return root;
	}
} // namespace aether::io
