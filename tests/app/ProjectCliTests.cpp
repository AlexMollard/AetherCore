#include <doctest/doctest.h>

#include "ProjectCli.hpp"

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
