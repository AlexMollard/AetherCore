#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "io/PlatformPaths.hpp"

using aether::io::PlatformPaths;

namespace
{
	void SetEnv(const char* name, const char* value)
	{
#ifdef _WIN32
		_putenv_s(name, value != nullptr ? value : "");
#else
		if (value != nullptr)
		{
			setenv(name, value, 1);
		}
		else
		{
			unsetenv(name);
		}
#endif
	}
}

TEST_CASE("ResolveToolExecutable resolves env override -> dev hint -> exe dir")
{
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path();
	const fs::path envFile = tmp / "ae_resolver_env.marker";
	const fs::path hintFile = tmp / "ae_resolver_hint.marker";
	std::ofstream(envFile) << 'x';
	std::ofstream(hintFile) << 'x';

	SUBCASE("env override wins when it exists")
	{
		SetEnv("AE_TEST_TOOL", envFile.string().c_str());
		const fs::path r = PlatformPaths::ResolveToolExecutable("AE_TEST_TOOL", hintFile.string(), "nope.exe");
		CHECK(fs::equivalent(r, envFile));
		SetEnv("AE_TEST_TOOL", nullptr);
	}

	SUBCASE("a set-but-missing env override is ignored, falling through to the dev hint")
	{
		SetEnv("AE_TEST_TOOL", (tmp / "ae_resolver_absent.marker").string().c_str());
		const fs::path r = PlatformPaths::ResolveToolExecutable("AE_TEST_TOOL", hintFile.string(), "nope.exe");
		CHECK(fs::equivalent(r, hintFile));
		SetEnv("AE_TEST_TOOL", nullptr);
	}

	SUBCASE("dev hint is used when the env is unset")
	{
		SetEnv("AE_TEST_TOOL", nullptr);
		const fs::path r = PlatformPaths::ResolveToolExecutable("AE_TEST_TOOL", hintFile.string(), "nope.exe");
		CHECK(fs::equivalent(r, hintFile));
	}

	SUBCASE("falls back to <exe dir>/fileName when env and hint are absent")
	{
		SetEnv("AE_TEST_TOOL", nullptr);
		const fs::path r = PlatformPaths::ResolveToolExecutable("AE_TEST_TOOL", (tmp / "ae_resolver_absent.marker").string(), "sentinel.exe");
		CHECK(r == PlatformPaths::GetExecutableDir() / "sentinel.exe");
	}

	SUBCASE("empty env name + empty hint go straight to the exe dir (shipped layout)")
	{
		const fs::path r = PlatformPaths::ResolveToolExecutable("", "", "sentinel.exe");
		CHECK(r == PlatformPaths::GetExecutableDir() / "sentinel.exe");
	}

	std::error_code ec;
	fs::remove(envFile, ec);
	fs::remove(hintFile, ec);
}
