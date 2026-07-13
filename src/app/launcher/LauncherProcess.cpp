#include "launcher/LauncherProcess.hpp"

#include <string>

#include "utils/Logger.hpp"

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#endif

// The Editor executable file name, injected by CMake ($<TARGET_FILE_NAME:Editor>).
// Only the Launcher target defines it; a fallback keeps this translation unit compiling
// in the Editor/GameRuntime targets (where SpawnEditor is never called).
#ifndef AETHER_EDITOR_EXE_NAME
#	define AETHER_EDITOR_EXE_NAME "Editor.exe"
#endif

namespace aether::app::launcher
{
#ifdef _WIN32
	namespace
	{
		std::filesystem::path ExecutableDir()
		{
			char buffer[MAX_PATH]{};
			const DWORD len = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
			if (len == 0 || len >= std::size(buffer))
			{
				return {};
			}
			return std::filesystem::path(std::string(buffer, len)).parent_path();
		}
	} // namespace

	bool SpawnEditor(const std::filesystem::path& projectRoot)
	{
		const std::filesystem::path dir = ExecutableDir();
		const std::filesystem::path editorExe = dir / AETHER_EDITOR_EXE_NAME;
		// CreateProcessA needs a mutable command-line buffer.
		std::string command = "\"" + editorExe.string() + "\" --project \"" + projectRoot.string() + "\"";

		STARTUPINFOA startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo{};
		const std::string workingDir = dir.string();
		const BOOL started = CreateProcessA(
		        nullptr, command.data(), nullptr, nullptr, FALSE,
		        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
		        workingDir.empty() ? nullptr : workingDir.c_str(), &startupInfo, &processInfo);
		if (started)
		{
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
			AE_INFO(LogCategory::App, "Launcher spawned Editor for project '{}'", projectRoot.string());
			return true;
		}
		AE_ERROR(LogCategory::App, "Launcher failed to spawn Editor (GetLastError={})", GetLastError());
		return false;
	}
#else
	bool SpawnEditor(const std::filesystem::path& projectRoot)
	{
		(void) projectRoot;
		AE_ERROR(LogCategory::App, "Launcher process spawn is only implemented on Windows.");
		return false;
	}
#endif
} // namespace aether::app::launcher
