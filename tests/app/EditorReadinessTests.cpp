#include <doctest/doctest.h>

#ifdef __linux__
#	include <string>
#	include <cerrno>
#	include <fcntl.h>
#	include <sys/socket.h>
#	include <sys/stat.h>
#	include <unistd.h>

#	include "EditorReadiness.hpp"

namespace
{
	struct ReadySockets
	{
		int fds[2]{-1, -1};

		~ReadySockets()
		{
			for (int fd: fds)
			{
				if (fd >= 0)
				{
					close(fd);
				}
			}
		}
	};
} // namespace

TEST_CASE("Editor readiness closes its one-shot socket and does not leak it into exec")
{
	ReadySockets sockets;
	REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets.fds) == 0);
	REQUIRE(aether::app::InitializeEditorReadiness(std::to_string(sockets.fds[1])));
	CHECK((fcntl(sockets.fds[1], F_GETFD) & FD_CLOEXEC) != 0);
	struct stat before{};
	REQUIRE(fstat(sockets.fds[1], &before) == 0);
	aether::app::SignalEditorReady();
	struct stat after{};
	const bool released = fstat(sockets.fds[1], &after) == -1 || before.st_ino != after.st_ino || before.st_dev != after.st_dev;
	CHECK(released);
	sockets.fds[1] = -1; // Ownership was transferred to the production readiness handler.
	char message = 0;
	CHECK(recv(sockets.fds[0], &message, 1, MSG_DONTWAIT) == 1);
	CHECK(message == 'R');
	const int unrelated = open("/dev/null", O_RDONLY);
	REQUIRE(unrelated >= 0);
	aether::app::SignalEditorReady();
	CHECK(fcntl(unrelated, F_GETFD) != -1);
	CHECK(recv(sockets.fds[0], &message, 1, MSG_DONTWAIT) == -1);
	const bool noMessage = errno == EAGAIN || errno == EWOULDBLOCK;
	CHECK(noMessage);
	close(unrelated);
}

TEST_CASE("Editor readiness rejects malformed descriptors and ordinary files without touching them")
{
	CHECK(aether::app::InitializeEditorReadiness(""));
	for (const auto value: {"0", "1", "2", "-1", "abc", "3suffix", "99999999999999999999"})
	{
		CHECK_FALSE(aether::app::InitializeEditorReadiness(value));
	}
	const int file = open("/dev/null", O_WRONLY);
	REQUIRE(file >= 0);
	CHECK_FALSE(aether::app::InitializeEditorReadiness(std::to_string(file)));
	CHECK(fcntl(file, F_GETFD) != -1);
	close(file);
}

TEST_CASE("Editor readiness survives a Launcher that closes before the editor becomes ready")
{
	ReadySockets sockets;
	REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets.fds) == 0);
	REQUIRE(aether::app::InitializeEditorReadiness(std::to_string(sockets.fds[1])));
	close(sockets.fds[0]);
	sockets.fds[0] = -1;
	struct stat before{};
	REQUIRE(fstat(sockets.fds[1], &before) == 0);
	aether::app::SignalEditorReady();
	struct stat after{};
	const bool released = fstat(sockets.fds[1], &after) == -1 || before.st_ino != after.st_ino || before.st_dev != after.st_dev;
	CHECK(released);
	sockets.fds[1] = -1;
}
#endif
