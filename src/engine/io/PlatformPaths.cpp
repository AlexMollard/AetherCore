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

	std::filesystem::path PlatformPaths::ResolveToolExecutable(std::string_view envVar, std::string_view devHint, std::string_view fileName)
	{
		std::error_code ec;
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

	std::filesystem::path PlatformPaths::GetUserConfigDir()
	{
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
} // namespace aether::io
