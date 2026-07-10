#include <doctest/doctest.h>

#include "io/Process.hpp"

using namespace aether;

TEST_CASE("WrapShellCommand makes cmd.exe quote-stripping a no-op on Windows")
{
	const std::string inner = "\"C:\\tools\\AssetPacker.exe\" --project \"C:\\my project\" \"C:\\out.pak\"";
	const std::string wrapped = io::WrapShellCommand(inner);
#ifdef _WIN32
	// Outer quote pair added so cmd's first/last-quote strip leaves `inner` intact.
	CHECK(wrapped.front() == '"');
	CHECK(wrapped.back() == '"');
	CHECK(wrapped == "\"" + inner + "\"");
#else
	CHECK(wrapped == inner);
#endif
}
