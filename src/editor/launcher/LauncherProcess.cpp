#include "launcher/LauncherProcess.hpp"

#include <climits>

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

// SpawnEditor is never called; an empty dev path just means "no hint", so the resolver
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
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort, int centerX, int centerY)
	{
		const std::filesystem::path editorExe = io::PlatformPaths::ResolveToolExecutable("AETHER_EDITOR_EXE", AETHER_EDITOR_EXE_PATH, AETHER_EDITOR_EXE_NAME);
		const std::filesystem::path dir = editorExe.parent_path();
		const std::string readyEventName = "Local\\AetherCoreEditorReady-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
		const HANDLE readyEvent = CreateEventA(nullptr, TRUE, FALSE, readyEventName.c_str());
		if (readyEvent == nullptr)
		{
			AE_ERROR(LogCategory::App, "Launcher could not create the Editor readiness event (GetLastError={})", GetLastError());
			return std::nullopt;
		}

		// Wide strings + CreateProcessW, matching the ShellExecuteW game-build spawn: a
		// project under a path the ANSI code page cannot represent (a Cyrillic/Greek/CJK
		// user name, say) survives wstring losslessly, while string() turns those
		// characters into '?' and the spawn fails or opens a mangled folder.
		const std::wstring wideReadyEventName(readyEventName.begin(), readyEventName.end());
		std::wstring command = L"\"" + editorExe.wstring() + L"\" --project \"" + projectRoot.wstring() + L"\" --ready-event \"" + wideReadyEventName + L"\"";
		if (centerX != INT_MIN && centerY != INT_MIN)
		{
			command += L" --window-center \"" + std::to_wstring(centerX) + L"," + std::to_wstring(centerY) + L"\"";
		}

		// The editor reads its control port from the inherited environment (see
		// ControlServerLayer), so the value must sit in OUR environment at CreateProcess
		// time - but only for this one child. Restore the previous value afterwards
		// (including on failure): leaving it set would hand a port the launcher no longer
		// owns to every later child this process spawns.
		const std::string previousControlPort = io::PlatformPaths::ReadEnvironmentVariable("AETHER_CONTROL_PORT");
		if (controlPort > 0)
		{
			SetEnvironmentVariableA("AETHER_CONTROL_PORT", std::to_string(controlPort).c_str());
		}

		STARTUPINFOW startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo{};
		const std::wstring workingDir = dir.wstring();
		const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, workingDir.empty() ? nullptr : workingDir.c_str(), &startupInfo, &processInfo);
		if (previousControlPort.empty())
		{
			SetEnvironmentVariableA("AETHER_CONTROL_PORT", nullptr);
		}
		else
		{
			SetEnvironmentVariableA("AETHER_CONTROL_PORT", previousControlPort.c_str());
		}
		if (started != 0)
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
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort, int centerX, int centerY)
	{
		(void) projectRoot;
		(void) controlPort;
		AE_ERROR(LogCategory::App, "Launcher process spawn is only implemented on Windows.");
		return std::nullopt;
	}
#endif
} // namespace aether::app::launcher
