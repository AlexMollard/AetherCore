#include <doctest/doctest.h>

#if defined(__linux__)
#	include <chrono>
#	include <cstdlib>
#	include <filesystem>
#	include <fstream>
#	include <optional>
#	include <string>
#	include <thread>
#	include <vector>
#	include <cerrno>
#	include <fcntl.h>
#	include <signal.h>
#	include <unistd.h>

#	include "launcher/LauncherProcess.hpp"

using namespace aether::app::launcher;

namespace
{
	struct LaunchEnvironment
	{
		std::vector<std::pair<std::string, std::optional<std::string>>> previous;

		void Set(const char* name, const std::string& value)
		{
			const char* old = std::getenv(name);
			previous.emplace_back(name, old != nullptr ? std::optional<std::string>(old) : std::nullopt);
			setenv(name, value.c_str(), 1);
		}

		~LaunchEnvironment()
		{
			for (auto it = previous.rbegin(); it != previous.rend(); ++it)
			{
				if (it->second)
				{
					setenv(it->first.c_str(), it->second->c_str(), 1);
				}
				else
				{
					unsetenv(it->first.c_str());
				}
			}
		}
	};

	template<typename Predicate>
	bool WaitUntil(Predicate predicate)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			if (predicate())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		while (std::chrono::steady_clock::now() < deadline);
		return predicate();
	}

	struct LaunchFixture
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / ("ae launcher Ω ' ; $ literal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		LaunchEnvironment environment;
		int childPid = 0;

		LaunchFixture()
		{
			std::filesystem::create_directories(root);
			environment.Set("AETHER_EDITOR_EXE", AETHER_LAUNCHER_PROBE);
			environment.Set("AETHER_ISOLATED_PRODUCT", "0");
			environment.Set("AETHER_CONTROL_PORT", "off");
			environment.Set("AETHER_LAUNCH_PROBE_MODE", "ready");
			environment.Set("AETHER_LAUNCH_PROBE_TOKEN", "literal Ω ; $ ' token");
		}

		void Allow(const char* name) const
		{
			std::ofstream(root / name) << "go";
		}

		std::vector<std::string> Report()
		{
			std::vector<std::string> lines;
			WaitUntil(
			        [&]
			        {
				        lines.clear();
				        std::ifstream input(root / "child.txt");
				        for (std::string line; std::getline(input, line);)
				        {
					        lines.push_back(line);
				        }
				        return lines.size() >= 10;
			        });
			if (!lines.empty())
			{
				childPid = std::stoi(lines[0]);
			}
			return lines;
		}

		bool Reaped() const
		{
			return childPid > 0 && kill(childPid, 0) == -1 && errno == ESRCH;
		}

		~LaunchFixture()
		{
			Allow("allow-ready");
			Allow("allow-exit");
			if (childPid > 0)
			{
				(void) WaitUntil([&] { return Reaped(); });
			}
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	};
} // namespace

TEST_CASE("Linux Launcher forwards literal arguments and child-only environment before readiness")
{
	LaunchFixture fixture;
	const auto originalCwd = std::filesystem::current_path();
	const int sourceFd = open("/dev/null", O_RDONLY);
	REQUIRE(sourceFd >= 0);
	const int unwanted = fcntl(sourceFd, F_DUPFD, 100);
	close(sourceFd);
	REQUIRE(unwanted >= 0);
	fixture.environment.Set("AETHER_LAUNCH_PROBE_UNWANTED_FD", std::to_string(unwanted));
	auto launch = SpawnEditor(fixture.root, 19387, -320, 640);
	close(unwanted);
	REQUIRE(launch.has_value());
	const auto report = fixture.Report();
	REQUIRE(report.size() == 14);
	CHECK(report[1] == std::to_string(getpid()));
	CHECK(report[2] == report[0]);
	CHECK(report[3] == std::filesystem::path(AETHER_LAUNCHER_PROBE).parent_path().string());
	CHECK(report[4] == "19387");
	CHECK(report[5] == "literal Ω ; $ ' token");
	CHECK(report[6] == "closed");
	CHECK(report[8] == "--project");
	CHECK(report[9] == fixture.root.string());
	CHECK(report[10] == "--ready-fd");
	CHECK(report[12] == "--window-center");
	CHECK(report[13] == "-320,640");
	CHECK(std::string(std::getenv("AETHER_CONTROL_PORT")) == "off");
	CHECK(std::filesystem::current_path() == originalCwd);
	CHECK(launch->Poll() == EditorStartupState::Pending);
	fixture.Allow("allow-ready");
	REQUIRE(WaitUntil([&] { return launch->Poll() == EditorStartupState::Ready; }));
	CHECK(launch->Poll() == EditorStartupState::Ready);
	fixture.Allow("allow-exit");
	REQUIRE(WaitUntil([&] { return launch->Poll() == EditorStartupState::Exited; }));
	CHECK(launch->Poll() == EditorStartupState::Exited);
	CHECK(fixture.Reaped());
}

TEST_CASE("Linux Launcher detects an editor that exits before signalling readiness")
{
	LaunchFixture fixture;
	fixture.environment.Set("AETHER_LAUNCH_PROBE_MODE", "exit");
	auto launch = SpawnEditor(fixture.root);
	REQUIRE(launch.has_value());
	const auto report = fixture.Report();
	REQUIRE(report.size() == 12);
	CHECK(report[4] == "off");
	REQUIRE(WaitUntil([&] { return launch->Poll() == EditorStartupState::Exited; }));
	CHECK(launch->Poll() == EditorStartupState::Exited);
	CHECK(fixture.Reaped());
}

TEST_CASE("Linux Launcher transfers process and readiness ownership on moves")
{
	LaunchFixture fixture;
	auto launch = SpawnEditor(fixture.root);
	REQUIRE(launch.has_value());
	REQUIRE(fixture.Report().size() == 12);
	EditorLaunch moved(std::move(*launch));
	CHECK(launch->Poll() == EditorStartupState::Exited);
	EditorLaunch assigned;
	assigned = std::move(moved);
	CHECK(moved.Poll() == EditorStartupState::Exited);
	fixture.Allow("allow-ready");
	REQUIRE(WaitUntil([&] { return assigned.Poll() == EditorStartupState::Ready; }));
	fixture.Allow("allow-exit");
	REQUIRE(WaitUntil([&] { return assigned.Poll() == EditorStartupState::Exited; }));
}

TEST_CASE("Linux Launcher release leaves a healthy child independent and reaps its eventual exit")
{
	LaunchFixture fixture;
	auto launch = SpawnEditor(fixture.root);
	REQUIRE(launch.has_value());
	REQUIRE(fixture.Report().size() == 12);
	launch.reset();
	CHECK(kill(fixture.childPid, 0) == 0);
	fixture.Allow("allow-ready");
	fixture.Allow("allow-exit");
	REQUIRE(WaitUntil([&] { return fixture.Reaped(); }));
}

TEST_CASE("Linux Launcher failed exec releases descriptors without changing parent state")
{
	LaunchFixture fixture;
	const auto invalid = fixture.root / "not executable";
	std::ofstream(invalid) << "not an ELF executable";
	fixture.environment.Set("AETHER_EDITOR_EXE", invalid.string());
	const auto originalCwd = std::filesystem::current_path();
	CHECK_FALSE(SpawnEditor(fixture.root, 19387).has_value());
	const auto descriptors = []
	{
		return std::distance(std::filesystem::directory_iterator("/proc/self/fd"), std::filesystem::directory_iterator{});
	};
	const auto before = descriptors();
	for (int i = 0; i < 8; ++i)
	{
		CHECK_FALSE(SpawnEditor(fixture.root, 19387).has_value());
	}
	CHECK(descriptors() == before);
	CHECK(std::filesystem::current_path() == originalCwd);
	CHECK(std::string(std::getenv("AETHER_CONTROL_PORT")) == "off");
}
#endif
