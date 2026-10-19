#include "debug/OpenInEditor.hpp"

#include <cstdlib>
#include <filesystem>

#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"

#ifdef _WIN32
#	include <windows.h>

#	include <shellapi.h>
#endif

namespace aether::editor
{
	void OpenInEditor(const std::string& filePath, int line)
	{
		if (filePath.empty())
		{
			return;
		}

		std::filesystem::path resolved = std::filesystem::u8path(filePath);
		if (!resolved.is_absolute())
		{
			std::error_code ec;
			auto candidate = std::filesystem::weakly_canonical(std::filesystem::current_path() / ".." / ".." / resolved, ec);
			if (!ec && io::file_util::Exists(candidate))
			{
				resolved = candidate;
			}
		}

#ifdef _WIN32
		std::wstring cmd;
		if (line > 0)
		{
			cmd = L"code -g \"" + resolved.wstring() + L":" + std::to_wstring(line) + L"\"";
		}
		else
		{
			cmd = L"code \"" + resolved.wstring() + L"\"";
		}

		const int ret = _wsystem((L"where code >nul 2>&1 && " + cmd).c_str());
		if (ret == 0)
		{
			return;
		}

		ShellExecuteW(nullptr, L"open", resolved.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
		std::string shellCmd = "code " + io::PlatformPaths::PathToUtf8(resolved);
		if (line > 0)
		{
			shellCmd += ":" + std::to_string(line);
		}
		system(shellCmd.c_str());
#endif
	}
} // namespace aether::editor
