#include "launcher/LauncherProcess.hpp"

#include <string>

#include "io/PlatformPaths.hpp"
#include "utils/Logger.hpp"

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>
#endif

// Editor executable name + build-injected dev path (CMake sets both on the Launcher
// target). Fallbacks keep this TU compiling in the Editor/GameRuntime targets, where
// SpawnEditor is never called; an empty dev path just means "no hint", so the resolver
// falls back to the launcher's own directory (the shipped layout).
#ifndef AETHER_EDITOR_EXE_NAME
#	define AETHER_EDITOR_EXE_NAME "Editor.exe"
#endif
#ifndef AETHER_EDITOR_EXE_PATH
#	define AETHER_EDITOR_EXE_PATH ""
#endif

namespace aether::app::launcher
{
#ifdef _WIN32
	bool SpawnEditor(const std::filesystem::path& projectRoot, int controlPort)
	{
		// Resolve the editor without assuming it sits next to us: env override
		// (AETHER_EDITOR_EXE) -> build-injected dev path -> our own directory.
		const std::filesystem::path editorExe = io::PlatformPaths::ResolveToolExecutable("AETHER_EDITOR_EXE", AETHER_EDITOR_EXE_PATH, AETHER_EDITOR_EXE_NAME);
		const std::filesystem::path dir = editorExe.parent_path();
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
