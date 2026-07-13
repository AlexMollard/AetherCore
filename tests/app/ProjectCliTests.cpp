#include <doctest/doctest.h>

#include "ProjectCli.hpp"

using aether::app::ParseProjectArg;

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
