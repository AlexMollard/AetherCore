#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv)
{
	std::filesystem::path project;
	int readyFd = -1;
	for (int i = 1; i + 1 < argc; ++i)
	{
		if (std::string_view(argv[i]) == "--project")
		{
			project = argv[i + 1];
		}
		if (std::string_view(argv[i]) == "--ready-fd")
		{
			readyFd = std::stoi(argv[i + 1]);
		}
	}
	if (project.empty() || readyFd < 3)
	{
		return 20;
	}
	{
		std::ofstream report(project / "child.txt");
		report << getpid() << '\n' << getppid() << '\n' << getpgrp() << '\n' << std::filesystem::current_path().string() << '\n';
		report << (std::getenv("AETHER_CONTROL_PORT") ? std::getenv("AETHER_CONTROL_PORT") : "<unset>") << '\n';
		report << (std::getenv("AETHER_LAUNCH_PROBE_TOKEN") ? std::getenv("AETHER_LAUNCH_PROBE_TOKEN") : "<unset>") << '\n';
		const char* unwanted = std::getenv("AETHER_LAUNCH_PROBE_UNWANTED_FD");
		report << (unwanted != nullptr && fcntl(std::stoi(unwanted), F_GETFD) != -1 ? "leaked" : "closed") << '\n';
		for (int i = 0; i < argc; ++i)
		{
			report << argv[i] << '\n';
		}
	}
	if (std::string_view(std::getenv("AETHER_LAUNCH_PROBE_MODE") ? std::getenv("AETHER_LAUNCH_PROBE_MODE") : "") == "exit")
	{
		return 13;
	}
	const auto waitFor = [&](const char* name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
		while (!std::filesystem::exists(project / name))
		{
			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return true;
	};
	if (!waitFor("allow-ready"))
	{
		return 21;
	}
	// Fixture side of the one-byte readiness protocol; no window or GPU is involved.
	(void) send(readyFd, "R", 1, MSG_NOSIGNAL);
	close(readyFd);
	return waitFor("allow-exit") ? 0 : 22;
}
