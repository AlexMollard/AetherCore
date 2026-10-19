#include <doctest/doctest.h>

#include "io/Process.hpp"
#include "io/FileUtil.hpp"
#include <array>
#include <chrono>
#include <stop_token>
#include <thread>

using namespace aether;

TEST_CASE("WrapShellCommand makes cmd.exe quote-stripping a no-op on Windows")
{
	const std::string inner = "\"C:\\tools\\AssetPacker.exe\" --project \"C:\\my project\" \"C:\\out.pak\"";
	const std::string wrapped = io::WrapShellCommand(inner);
#ifdef _WIN32
	CHECK(wrapped.front() == '"');
	CHECK(wrapped.back() == '"');
	CHECK(wrapped == "\"" + inner + "\"");
#else
	CHECK(wrapped == inner);
#endif
}

TEST_CASE("Direct process arguments preserve shell syntax and UTF8 literally")
{
	const auto root = std::filesystem::temp_directory_path() / "aether direct process";
	std::filesystem::create_directories(root);
	const std::string literal = "a & b | $(echo bad) %PATH% ; quote\" \\ caf\xc3\xa9";
	const std::array<std::string, 3> args{"-E", "echo", literal};
	REQUIRE(io::RunProcessArguments(AETHER_TEST_CMAKE, args, root, root / "output.log", 5000) == 0);
	const auto text = io::file_util::ReadText(root / "output.log");
	REQUIRE(text);
	CHECK(text->starts_with(literal));
	CHECK(io::RunProcessArguments(root / "does not exist", args, root, root / "missing.log", 5000) == io::kProcessLaunchFailed);
	const std::array<std::string, 3> sleep{"-E", "sleep", "10"};
	CHECK(io::RunProcessArguments(AETHER_TEST_CMAKE, sleep, root, root / "timeout.log", 50) == io::kProcessTimedOut);
	std::filesystem::remove_all(root);
}

TEST_CASE("Direct process cancellation before launch leaves the log untouched")
{
	const auto root = std::filesystem::temp_directory_path() / "aether pre-cancel process";
	std::filesystem::remove_all(root);
	const std::array<std::string, 3> args{"-E", "echo", "must not run"};
	std::stop_source cancellation;
	cancellation.request_stop();
	CHECK(io::RunProcessArguments(AETHER_TEST_CMAKE, args, root, root / "output.log", 5000, cancellation.get_token()) == io::kProcessCancelled);
	CHECK_FALSE(std::filesystem::exists(root));
}

TEST_CASE("Direct process cancellation stops a running child before its timeout")
{
	const auto root = std::filesystem::temp_directory_path() / "aether cancelled process";
	std::filesystem::create_directories(root);
	const std::array<std::string, 3> args{"-E", "sleep", "10"};
	std::stop_source cancellation;
	std::jthread cancel([&cancellation]
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		cancellation.request_stop();
	});
	const auto start = std::chrono::steady_clock::now();
	CHECK(io::RunProcessArguments(AETHER_TEST_CMAKE, args, root, root / "output.log", 5000, cancellation.get_token()) == io::kProcessCancelled);
	CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
	std::filesystem::remove_all(root);
}
