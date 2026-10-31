#include <doctest/doctest.h>

#include "ProjectCli.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

using aether::app::ParseProjectArg;
using aether::app::ResolveValidationEnabled;

TEST_CASE("ParseProjectArg returns the path after --project")
{
	const char* argv[] = {"Editor", "--project", "D:/proj/Test"};
	CHECK(ParseProjectArg(3, const_cast<char**>(argv)) == "D:/proj/Test");
}

TEST_CASE("ParseProjectArg returns empty when the flag is absent or has no value")
{
	const char* none[] = {"Editor"};
	CHECK(ParseProjectArg(1, const_cast<char**>(none)).empty());

	const char* noValue[] = {"Editor", "--project"};
	CHECK(ParseProjectArg(2, const_cast<char**>(noValue)).empty());
}

TEST_CASE("Native wide project arguments preserve non-ACP paths on ordinary launches")
{
	// No --agent flag: this is the normal Launcher -> Editor command line. Omega
	// and CJK cannot round trip through Windows ACP 1252's narrow argv spelling.
	const wchar_t* argv[] = {L"Editor", L"--project", L"C:/Projects/Reference \u03a9 \u5305", L"--validation"};
	CHECK(ParseProjectArg(4, const_cast<wchar_t**>(argv)) == L"C:/Projects/Reference \u03a9 \u5305");
	const wchar_t* missing[] = {L"Editor", L"--project"};
	CHECK(ParseProjectArg(2, const_cast<wchar_t**>(missing)).empty());
	const wchar_t* none[] = {L"Editor"};
	CHECK(ParseProjectArg(1, const_cast<wchar_t**>(none)).empty());
}

#ifdef _WIN32
TEST_CASE("Quoted normal Windows launcher project paths remain native Unicode")
{
	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(L"Editor.exe --project \"C:\\Projects\\Reference \u03a9 \u5305\" --validation", &argc);
	REQUIRE(argv != nullptr);
	const auto project = ParseProjectArg(argc, argv);
	LocalFree(argv);
	CHECK(argc == 4);
	CHECK(project == L"C:\\Projects\\Reference \u03a9 \u5305");
}
#endif

// Whether the Vulkan validation layer is on is now build-config dependent: Debug defaults
// on, RelWithDebInfo defaults off. Both defaults are passed in rather than baked in here,
// so these cases pin the resolution policy itself and hold whichever config runs them.
TEST_CASE("With no flags, validation follows the build default")
{
	const char* argv[] = {"Editor", "--project", "D:/proj/Test"};
	CHECK(ResolveValidationEnabled(3, const_cast<char**>(argv), true));
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(argv), false));
}

TEST_CASE("--validation turns the layer on from a build that defaults it off")
{
	// The RelWithDebInfo opt-in: this is the only way back to a validated session in the
	// config the editor is normally used in.
	const char* argv[] = {"Editor", "--validation"};
	CHECK(ResolveValidationEnabled(2, const_cast<char**>(argv), false));
	CHECK(ResolveValidationEnabled(2, const_cast<char**>(argv), true));
}

TEST_CASE("--no-validation turns the layer off from a build that defaults it on")
{
	// The Debug opt-out, and the switch that hands the run to Aftermath instead.
	const char* argv[] = {"Editor", "--no-validation"};
	CHECK_FALSE(ResolveValidationEnabled(2, const_cast<char**>(argv), true));
	CHECK_FALSE(ResolveValidationEnabled(2, const_cast<char**>(argv), false));
}

TEST_CASE("--no-validation beats --validation whichever order they appear in")
{
	// Off is the escape hatch, so it must not be out-rankable by a wrapper script that
	// also passes --validation. Order must not matter either: a shell alias appends, and
	// whether the appended flag lands before or after is not something a caller controls.
	const char* offLast[] = {"Editor", "--validation", "--no-validation"};
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(offLast), true));
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(offLast), false));

	const char* offFirst[] = {"Editor", "--no-validation", "--validation"};
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(offFirst), true));
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(offFirst), false));
}

TEST_CASE("argv[0] is skipped rather than matched as a flag")
{
	// The scan starts at 1 because argv[0] is the program name, not an argument. Spelled
	// exactly so this actually exercises the loop bound: a path merely *containing* the
	// text is rejected by the whole-argument comparison instead, which is a different
	// guard and is covered by the near-miss case below.
	const char* offAsProgramName[] = {"--no-validation", "--project", "D:/proj/Test"};
	CHECK(ResolveValidationEnabled(3, const_cast<char**>(offAsProgramName), true));

	const char* onAsProgramName[] = {"--validation", "--project", "D:/proj/Test"};
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(onAsProgramName), false));
}

TEST_CASE("A path that merely contains the flag text is not a flag")
{
	// Comparison is whole-argument, so a build directory that happens to spell one of
	// these cannot decide the mode.
	const char* argv[] = {"Editor", "--project", "D:/build/--no-validation/Test"};
	CHECK(ResolveValidationEnabled(3, const_cast<char**>(argv), true));
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(argv), false));
}

TEST_CASE("A near-miss spelling is not treated as the flag")
{
	// Exact match only: a typo must fall through to the build default rather than
	// silently turning the layer off in a Debug session that was relying on it.
	const char* argv[] = {"Editor", "--novalidation", "--validation=false"};
	CHECK(ResolveValidationEnabled(3, const_cast<char**>(argv), true));
	CHECK_FALSE(ResolveValidationEnabled(3, const_cast<char**>(argv), false));
}

TEST_CASE("Agent launch requires an explicit name project and valid port")
{
	const char* valid[] = {"Editor", "--agent", "--agent-name", "Scene review", "--project", "D:/my project", "--control-port", "9011"};
	const auto options = aether::app::ParseAgentLaunchOptions(8, const_cast<char**>(valid));
	CHECK(options.error.empty());
	CHECK(options.enabled);
	CHECK(options.name == "Scene review");
	CHECK(options.controlPort == 9011);
	for (const char* invalidPort: {"0", "65536", "9011junk", "-1", ""})
	{
		const char* args[] = {"Editor", "--agent", "--agent-name", "review", "--project", "D:/project", "--control-port", invalidPort};
		CHECK_FALSE(aether::app::ParseAgentLaunchOptions(8, const_cast<char**>(args)).error.empty());
	}
}

TEST_CASE("Agent launch rejects missing blank duplicate and orphaned identity options")
{
	const char* missingName[] = {"Editor", "--agent", "--project", "D:/project", "--control-port", "9011"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(6, const_cast<char**>(missingName)).error.empty());
	const char* blankName[] = {"Editor", "--agent", "--agent-name", " \t", "--project", "D:/project", "--control-port", "9011"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(8, const_cast<char**>(blankName)).error.empty());
	const char* missingProject[] = {"Editor", "--agent", "--agent-name", "review", "--control-port", "9011"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(6, const_cast<char**>(missingProject)).error.empty());
	const char* missingPort[] = {"Editor", "--agent", "--agent-name", "review", "--project", "D:/project"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(6, const_cast<char**>(missingPort)).error.empty());
	const char* duplicateName[] = {"Editor", "--agent", "--agent-name", "review", "--agent-name", "other", "--project", "D:/project", "--control-port", "9011"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(10, const_cast<char**>(duplicateName)).error.empty());
	const char* orphanName[] = {"Editor", "--agent-name", "review"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(3, const_cast<char**>(orphanName)).error.empty());
}

TEST_CASE("Agent options reject duplicates and flags consumed as missing values")
{
	const char* flagAsName[] = {"Editor", "--agent", "--agent-name", "--project", "D:/project", "--control-port", "9011"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(7, const_cast<char**>(flagAsName)).error.empty());
	const char* duplicateProject[] = {"Editor", "--agent", "--agent-name", "review", "--project", "D:/project", "--project", "D:/other", "--control-port", "9011"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(10, const_cast<char**>(duplicateProject)).error.empty());
	const char* newlineName[] = {"Editor", "--agent", "--agent-name", "review\nother", "--project", "D:/project", "--control-port", "9011"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(8, const_cast<char**>(newlineName)).error.empty());
}
TEST_CASE("Interactive launches keep their existing behavior")
{
	const char* args[] = {"Editor", "--project", "D:/project", "--validation"};
	const auto options = aether::app::ParseAgentLaunchOptions(4, const_cast<char**>(args));
	CHECK_FALSE(options.enabled);
	CHECK(options.error.empty());
}
TEST_CASE("Agent background launch is explicit and game agents do not require a control port")
{
	const char* editor[] = {"Editor", "--agent", "--background", "--agent-name", "hidden", "--project", "D:/project", "--control-port", "9011"};
	CHECK(aether::app::ParseAgentLaunchOptions(9, const_cast<char**>(editor)).background);
	const char* game[] = {"Game", "--agent", "--background", "--agent-name", "hidden game", "--project", "D:/project"};
	const auto gameLaunch = aether::app::ParseAgentLaunchOptions(7, const_cast<char**>(game), false);
	CHECK(gameLaunch.error.empty());
	CHECK(gameLaunch.background);
	CHECK(gameLaunch.controlPort == 0);
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(7, const_cast<char**>(game)).error.empty());
	const char* orphan[] = {"Editor", "--background"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(2, const_cast<char**>(orphan)).error.empty());
}
TEST_CASE("Packaged game agents use baked project settings when no project override is supplied")
{
	const char* packaged[] = {"Game", "--agent", "--agent-name", "packaged game", "--background"};
	const auto options = aether::app::ParseAgentLaunchOptions(5, const_cast<char**>(packaged), false);
	CHECK(options.error.empty());
	CHECK(options.enabled);
	CHECK(options.background);
	CHECK(options.controlPort == 0);
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(5, const_cast<char**>(packaged)).error.empty());
	const char* blankOverride[] = {"Game", "--agent", "--agent-name", "packaged game", "--project", ""};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(6, const_cast<char**>(blankOverride), false).error.empty());
	const char* missingOverride[] = {"Game", "--agent", "--agent-name", "packaged game", "--project"};
	CHECK_FALSE(aether::app::ParseAgentLaunchOptions(5, const_cast<char**>(missingOverride), false).error.empty());
}
