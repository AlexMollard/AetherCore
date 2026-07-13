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

	bool SpawnEditor(const std::filesystem::path& projectRoot, int controlPort)
	{
		const std::filesystem::path dir = ExecutableDir();
		const std::filesystem::path editorExe = dir / AETHER_EDITOR_EXE_NAME;
		// CreateProcessA needs a mutable command-line buffer.
		std::string command = "\"" + editorExe.string() + "\" --project \"" + projectRoot.string() + "\"";

		// Forward the MCP control port to the child by overriding AETHER_CONTROL_PORT in
		// THIS process's environment just before the spawn: CreateProcessA with a null
		// lpEnvironment gives the child a copy of the current environment block, so it
		// inherits the value we set here. Mutating our own copy is harmless - the
		// launcher runs no ControlServer and does not read the var after startup. Each
		// spawn overwrites it, so per-editor incrementing ports (LauncherLayer) work.
		if (controlPort > 0)
		{
			SetEnvironmentVariableA("AETHER_CONTROL_PORT", std::to_string(controlPort).c_str());
		}

		STARTUPINFOA startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo{};
		const std::string workingDir = dir.string();
		const BOOL started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, workingDir.empty() ? nullptr : workingDir.c_str(), &startupInfo, &processInfo);
		if (started)
		{
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
			if (controlPort > 0)
			{
				AE_INFO(LogCategory::App, "Launcher spawned Editor for project '{}' (MCP control port {})", projectRoot.string(), controlPort);
			}
			else
			{
				AE_INFO(LogCategory::App, "Launcher spawned Editor for project '{}'", projectRoot.string());
			}
			return true;
		}
		AE_ERROR(LogCategory::App, "Launcher failed to spawn Editor (GetLastError={})", GetLastError());
		return false;
	}
#else
	bool SpawnEditor(const std::filesystem::path& projectRoot, int controlPort)
	{
		(void) projectRoot;
		(void) controlPort;
		AE_ERROR(LogCategory::App, "Launcher process spawn is only implemented on Windows.");
		return false;
	}
#endif
} // namespace aether::app::launcher
