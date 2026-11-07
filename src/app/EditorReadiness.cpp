#include "EditorReadiness.hpp"

#include <utility>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#elif defined(__linux__)
#	include <charconv>
#	include <cerrno>
#	include <cstring>
#	include <fcntl.h>
#	include <sys/socket.h>
#	include <unistd.h>
#endif

#include "io/PlatformPaths.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
#ifdef __linux__
	namespace
	{
		struct ReadySocket
		{
			int fd = -1;

			~ReadySocket()
			{
				if (fd >= 0)
				{
					close(fd);
				}
			}
		} s_readySocket;
	} // namespace
#endif

	bool InitializeEditorReadiness(std::string_view descriptor)
	{
		if (descriptor.empty())
		{
			return true;
		}
#ifdef __linux__
		int fd = -1;
		const auto [end, error] = std::from_chars(descriptor.data(), descriptor.data() + descriptor.size(), fd);
		if (error != std::errc{} || end != descriptor.data() + descriptor.size() || fd < 3)
		{
			return false;
		}
		int type = 0;
		socklen_t size = sizeof(type);
		sockaddr address{};
		socklen_t addressSize = sizeof(address);
		if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_DGRAM || getpeername(fd, &address, &addressSize) != 0 || address.sa_family != AF_UNIX)
		{
			return false;
		}
		const int flags = fcntl(fd, F_GETFD);
		if (flags == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
		{
			return false;
		}
		if (s_readySocket.fd >= 0 && s_readySocket.fd != fd)
		{
			close(s_readySocket.fd);
		}
		s_readySocket.fd = fd;
		return true;
#else
		return false;
#endif
	}

	void SignalEditorReady()
	{
#ifdef _WIN32
		const std::string eventName = io::PlatformPaths::ReadEnvironmentVariable("AETHER_EDITOR_READY_EVENT");
		if (eventName.empty())
		{
			return;
		}
		if (const HANDLE eventHandle = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName.c_str()))
		{
			SetEvent(eventHandle);
			CloseHandle(eventHandle);
			AE_INFO(LogCategory::App, "Editor startup readiness confirmed to Launcher.");
		}
		else
		{
			AE_WARN(LogCategory::App, "Could not signal Editor startup readiness (GetLastError={}).", GetLastError());
		}
#elif defined(__linux__)
		const int fd = std::exchange(s_readySocket.fd, -1);
		if (fd < 0)
		{
			return;
		}
		ssize_t sent = 0;
		do
		{
			sent = send(fd, "R", 1, MSG_NOSIGNAL);
		}
		while (sent == -1 && errno == EINTR);
		const int error = errno;
		close(fd);
		if (sent == 1)
		{
			AE_INFO(LogCategory::App, "Editor startup readiness confirmed to Launcher.");
		}
		else
		{
			AE_WARN(LogCategory::App, "Could not signal Editor startup readiness: {}", std::strerror(error));
		}
#endif
	}
} // namespace aether::app
