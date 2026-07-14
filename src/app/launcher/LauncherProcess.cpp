#include "launcher/LauncherProcess.hpp"

#include <string>
#include <utility>

#include "io/PlatformPaths.hpp"
#include "utils/Logger.hpp"

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
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
	EditorLaunch::EditorLaunch(std::uintptr_t processHandle, std::uintptr_t readyEventHandle)
	      : m_processHandle(processHandle), m_readyEventHandle(readyEventHandle)
	{
	}

	EditorLaunch::~EditorLaunch()
	{
		Reset();
	}

	EditorLaunch::EditorLaunch(EditorLaunch&& other) noexcept
	      : m_processHandle(std::exchange(other.m_processHandle, 0)), m_readyEventHandle(std::exchange(other.m_readyEventHandle, 0))
	{
	}

	EditorLaunch& EditorLaunch::operator=(EditorLaunch&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_processHandle = std::exchange(other.m_processHandle, 0);
			m_readyEventHandle = std::exchange(other.m_readyEventHandle, 0);
		}
		return *this;
	}

	void EditorLaunch::Reset()
	{
#ifdef _WIN32
		if (m_processHandle != 0)
		{
			CloseHandle(reinterpret_cast<HANDLE>(m_processHandle));
		}
		if (m_readyEventHandle != 0)
		{
			CloseHandle(reinterpret_cast<HANDLE>(m_readyEventHandle));
		}
#endif
		m_processHandle = 0;
		m_readyEventHandle = 0;
	}

	EditorStartupState EditorLaunch::Poll() const
	{
#ifdef _WIN32
		const HANDLE process = reinterpret_cast<HANDLE>(m_processHandle);
		if (process == nullptr || WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
		{
			return EditorStartupState::Exited;
		}
		const HANDLE readyEvent = reinterpret_cast<HANDLE>(m_readyEventHandle);
		return readyEvent != nullptr && WaitForSingleObject(readyEvent, 0) == WAIT_OBJECT_0 ? EditorStartupState::Ready : EditorStartupState::Pending;
#else
		return EditorStartupState::Exited;
#endif
	}

#ifdef _WIN32
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort)
	{
		// Resolve the editor without assuming it sits next to us: env override
		// (AETHER_EDITOR_EXE) -> build-injected dev path -> our own directory.
		const std::filesystem::path editorExe = io::PlatformPaths::ResolveToolExecutable("AETHER_EDITOR_EXE", AETHER_EDITOR_EXE_PATH, AETHER_EDITOR_EXE_NAME);
		const std::filesystem::path dir = editorExe.parent_path();
		const std::string readyEventName = "Local\\AetherCoreEditorReady-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
		const HANDLE readyEvent = CreateEventA(nullptr, TRUE, FALSE, readyEventName.c_str());
		if (readyEvent == nullptr)
		{
			AE_ERROR(LogCategory::App, "Launcher could not create the Editor readiness event (GetLastError={})", GetLastError());
			return std::nullopt;
		}

		// CreateProcessA needs a mutable command-line buffer.
		std::string command = "\"" + editorExe.string() + "\" --project \"" + projectRoot.string() + "\" --ready-event \"" + readyEventName + "\"";

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
			CloseHandle(processInfo.hThread);
			if (controlPort > 0)
			{
				AE_INFO(LogCategory::App, "Launcher spawned Editor for project '{}' (MCP control port {})", projectRoot.string(), controlPort);
			}
			else
			{
				AE_INFO(LogCategory::App, "Launcher spawned Editor for project '{}'", projectRoot.string());
			}
			return EditorLaunch(reinterpret_cast<std::uintptr_t>(processInfo.hProcess), reinterpret_cast<std::uintptr_t>(readyEvent));
		}
		AE_ERROR(LogCategory::App, "Launcher failed to spawn Editor (GetLastError={})", GetLastError());
		CloseHandle(readyEvent);
		return std::nullopt;
	}
#else
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort)
	{
		(void) projectRoot;
		(void) controlPort;
		AE_ERROR(LogCategory::App, "Launcher process spawn is only implemented on Windows.");
		return std::nullopt;
	}
#endif
} // namespace aether::app::launcher
