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

// ResolveBundlePath answers "where is the content the engine ships": the source tree
// during development, the copy staged beside the executable once packaged. The case that
// matters for a release is the one a developer's machine can never hit by accident - the
// dev hint pointing at a build machine's disk that simply is not there.
TEST_CASE("ResolveBundlePath resolves env override -> dev hint -> staged bundle copy")
{
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path();
	const fs::path envFile = tmp / "ae_bundle_env.marker";
	const fs::path hintFile = tmp / "ae_bundle_hint.marker";
	const fs::path absent = tmp / "ae_bundle_absent.marker";
	std::ofstream(envFile) << 'x';
	std::ofstream(hintFile) << 'x';

	// The staged copy has to live under the executable's own directory, because that is
	// the whole point of it - so the test puts a real file there and takes it away again.
	const fs::path stagedRelative = fs::path("ae_bundle_test") / "staged.marker";
	const fs::path stagedAbsolute = PlatformPaths::GetExecutableDir() / stagedRelative;
	std::error_code ec;
	fs::create_directories(stagedAbsolute.parent_path(), ec);
	std::ofstream(stagedAbsolute) << 'x';

	SUBCASE("env override wins over both")
	{
		SetEnv("AE_TEST_BUNDLE", envFile.string().c_str());
		const fs::path r = PlatformPaths::ResolveBundlePath("AE_TEST_BUNDLE", hintFile.string(), stagedRelative.string());
		CHECK(fs::equivalent(r, envFile));
		SetEnv("AE_TEST_BUNDLE", nullptr);
	}

	SUBCASE("the dev hint wins over the staged copy, so a developer edits the tree they are in")
	{
		SetEnv("AE_TEST_BUNDLE", nullptr);
		const fs::path r = PlatformPaths::ResolveBundlePath("AE_TEST_BUNDLE", hintFile.string(), stagedRelative.string());
		CHECK(fs::equivalent(r, hintFile));
	}

	SUBCASE("a packaged build has no dev hint and finds the staged copy")
	{
		SetEnv("AE_TEST_BUNDLE", nullptr);
		const fs::path r = PlatformPaths::ResolveBundlePath("AE_TEST_BUNDLE", "", stagedRelative.string());
		CHECK(fs::equivalent(r, stagedAbsolute));
	}

	SUBCASE("a dev hint naming a path off this machine falls through to the staged copy")
	{
		SetEnv("AE_TEST_BUNDLE", nullptr);
		const fs::path r = PlatformPaths::ResolveBundlePath("AE_TEST_BUNDLE", absent.string(), stagedRelative.string());
		CHECK(fs::equivalent(r, stagedAbsolute));
	}

	SUBCASE("nothing anywhere is EMPTY, not a plausible-looking guess")
	{
		SetEnv("AE_TEST_BUNDLE", nullptr);
		const fs::path r = PlatformPaths::ResolveBundlePath("AE_TEST_BUNDLE", absent.string(), "ae_bundle_test/missing.marker");
		CHECK(r.empty());
	}

	fs::remove(envFile, ec);
	fs::remove(hintFile, ec);
	fs::remove_all(stagedAbsolute.parent_path(), ec);
}
