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
#elif defined(__linux__)
#	include <atomic>
#	include <cerrno>
#	include <cstring>
#	include <vector>
#	include <pthread.h>
#	include <spawn.h>
#	include <signal.h>
#	include <sys/socket.h>
#	include <sys/wait.h>
#	include <unistd.h>

extern char** environ;
#endif

// SpawnEditor is never called; an empty dev path just means "no hint", so the resolver
#ifndef AETHER_EDITOR_EXE_NAME
#	ifdef _WIN32
#		define AETHER_EDITOR_EXE_NAME "Editor.exe"
#	else
#		define AETHER_EDITOR_EXE_NAME "Editor"
#	endif
#endif
#ifndef AETHER_EDITOR_EXE_PATH
#	define AETHER_EDITOR_EXE_PATH ""
#endif

namespace aether::app::launcher
{
#ifdef __linux__
	struct EditorLaunch::LinuxProcess
	{
		int readySocket = -1;
		std::shared_ptr<std::atomic<bool>> exited = std::make_shared<std::atomic<bool>>(false);

		~LinuxProcess()
		{
			if (readySocket >= 0)
			{
				close(readySocket);
			}
		}
	};

	namespace
	{
		struct EditorExitWait
		{
			pid_t pid = -1;
			std::shared_ptr<std::atomic<bool>> exited;
		};

		void* ReapEditor(void* pointer)
		{
			const std::unique_ptr<EditorExitWait> wait(static_cast<EditorExitWait*>(pointer));
			int status = 0;
			while (waitpid(wait->pid, &status, 0) == -1 && errno == EINTR)
			{
			}
			wait->exited->store(true, std::memory_order_release);
			return nullptr;
		}

		struct SpawnResources
		{
			int sockets[2]{-1, -1};
			posix_spawn_file_actions_t actions{};
			posix_spawnattr_t attributes{};
			pthread_attr_t waiterAttributes{};
			bool actionsInitialized = false;
			bool attributesInitialized = false;
			bool waiterInitialized = false;

			~SpawnResources()
			{
				for (const int fd: sockets)
				{
					if (fd >= 0)
					{
						close(fd);
					}
				}
				if (actionsInitialized)
				{
					posix_spawn_file_actions_destroy(&actions);
				}
				if (attributesInitialized)
				{
					posix_spawnattr_destroy(&attributes);
				}
				if (waiterInitialized)
				{
					pthread_attr_destroy(&waiterAttributes);
				}
			}
		};
	} // namespace
#endif

	EditorLaunch::EditorLaunch() = default;

	EditorLaunch::EditorLaunch(std::uintptr_t processHandle, std::uintptr_t readyEventHandle)
	      : m_processHandle(processHandle), m_readyEventHandle(readyEventHandle)
	{
	}

	EditorLaunch::~EditorLaunch()
	{
		Reset();
	}

	EditorLaunch::EditorLaunch(EditorLaunch&& other) noexcept
	      : m_processHandle(std::exchange(other.m_processHandle, 0)),
	        m_readyEventHandle(std::exchange(other.m_readyEventHandle, 0))
#ifdef __linux__
	        ,
	        m_linuxProcess(std::move(other.m_linuxProcess))
#endif
	{
	}

	EditorLaunch& EditorLaunch::operator=(EditorLaunch&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_processHandle = std::exchange(other.m_processHandle, 0);
			m_readyEventHandle = std::exchange(other.m_readyEventHandle, 0);
#ifdef __linux__
			m_linuxProcess = std::move(other.m_linuxProcess);
#endif
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
#ifdef __linux__
		m_linuxProcess.reset();
#endif
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
#elif defined(__linux__)
		if (!m_linuxProcess || m_linuxProcess->exited->load(std::memory_order_acquire))
		{
			return EditorStartupState::Exited;
		}
		char signal = 0;
		const auto received = recv(m_linuxProcess->readySocket, &signal, 1, MSG_PEEK | MSG_DONTWAIT);
		return received == 1 && signal == 'R' ? EditorStartupState::Ready : EditorStartupState::Pending;
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
#elif defined(__linux__)
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort, int centerX, int centerY)
	{
		const auto resolved = io::PlatformPaths::ResolveToolExecutable("AETHER_EDITOR_EXE", AETHER_EDITOR_EXE_PATH, AETHER_EDITOR_EXE_NAME);
		if (resolved.empty())
		{
			AE_ERROR(LogCategory::App, "Launcher could not find the Editor executable.");
			return std::nullopt;
		}
		const auto editorExe = std::filesystem::absolute(resolved);
		constexpr int kChildReadyFd = 3;
		std::vector<std::string> arguments{io::PlatformPaths::PathToUtf8(editorExe), "--project", io::PlatformPaths::PathToUtf8(std::filesystem::absolute(projectRoot)), "--ready-fd", std::to_string(kChildReadyFd)};
		if (centerX != INT_MIN && centerY != INT_MIN)
		{
			arguments.emplace_back("--window-center");
			arguments.push_back(std::to_string(centerX) + "," + std::to_string(centerY));
		}
		std::vector<char*> argv;
		for (auto& argument: arguments)
		{
			argv.push_back(argument.data());
		}
		argv.push_back(nullptr);

		// Construct only this child's environment; the Launcher's port/cwd never change.
		std::vector<std::string> environment;
		for (char** entry = environ; *entry != nullptr; ++entry)
		{
			if (controlPort <= 0 || !std::string_view(*entry).starts_with("AETHER_CONTROL_PORT="))
			{
				environment.emplace_back(*entry);
			}
		}
		if (controlPort > 0)
		{
			environment.push_back("AETHER_CONTROL_PORT=" + std::to_string(controlPort));
		}
		std::vector<char*> envp;
		for (auto& entry: environment)
		{
			envp.push_back(entry.data());
		}
		envp.push_back(nullptr);

		auto process = std::make_unique<EditorLaunch::LinuxProcess>();
		auto wait = std::make_unique<EditorExitWait>();
		wait->exited = process->exited;
		SpawnResources resources;
		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, resources.sockets) != 0)
		{
			AE_ERROR(LogCategory::App, "Launcher could not create the Editor readiness socket: {}", std::strerror(errno));
			return std::nullopt;
		}
		int error = posix_spawn_file_actions_init(&resources.actions);
		resources.actionsInitialized = error == 0;
		if (error == 0)
		{
			error = posix_spawnattr_init(&resources.attributes);
			resources.attributesInitialized = error == 0;
		}
		if (error == 0)
		{
			error = pthread_attr_init(&resources.waiterAttributes);
			resources.waiterInitialized = error == 0;
		}
		if (error == 0)
		{
			error = pthread_attr_setdetachstate(&resources.waiterAttributes, PTHREAD_CREATE_DETACHED);
		}
		if (error == 0)
		{
			error = posix_spawnattr_setpgroup(&resources.attributes, 0);
		}
		if (error == 0)
		{
			error = posix_spawnattr_setflags(&resources.attributes, POSIX_SPAWN_SETPGROUP);
		}
		if (error == 0)
		{
			error = posix_spawn_file_actions_addchdir_np(&resources.actions, editorExe.parent_path().c_str());
		}
		// dup2's spawn action clears CLOEXEC even when the source already occupies fd 3.
		if (error == 0)
		{
			error = posix_spawn_file_actions_adddup2(&resources.actions, resources.sockets[1], kChildReadyFd);
		}
		if (error == 0)
		{
			error = posix_spawn_file_actions_addclosefrom_np(&resources.actions, kChildReadyFd + 1);
		}
		if (error == 0)
		{
			error = posix_spawn(&wait->pid, editorExe.c_str(), &resources.actions, &resources.attributes, argv.data(), envp.data());
		}
		if (error != 0)
		{
			AE_ERROR(LogCategory::App, "Launcher failed to spawn Editor: {}", std::strerror(error));
			return std::nullopt;
		}

		pthread_t waiter{};
		error = pthread_create(&waiter, &resources.waiterAttributes, ReapEditor, wait.get());
		if (error != 0)
		{
			// We still own this exact unreaped child; never leave it running untracked.
			kill(wait->pid, SIGKILL);
			int status = 0;
			while (waitpid(wait->pid, &status, 0) == -1 && errno == EINTR)
			{
			}
			AE_ERROR(LogCategory::App, "Launcher could not track the Editor process: {}", std::strerror(error));
			return std::nullopt;
		}
		(void) wait.release(); // The detached waiter owns only exit state, not readiness fds.
		process->readySocket = std::exchange(resources.sockets[0], -1);
		EditorLaunch launch;
		launch.m_linuxProcess = std::move(process);
		AE_INFO(LogCategory::App, "Launcher spawned Editor for project '{}' (MCP control port {})", io::PlatformPaths::PathToUtf8(projectRoot), controlPort);
		return launch;
	}
#else
	std::optional<EditorLaunch> SpawnEditor(const std::filesystem::path& projectRoot, int controlPort, int centerX, int centerY)
	{
		(void) projectRoot;
		(void) controlPort;
		(void) centerX;
		(void) centerY;
		AE_ERROR(LogCategory::App, "Launcher process spawn is unsupported on this platform.");
		return std::nullopt;
	}
#endif
} // namespace aether::app::launcher
