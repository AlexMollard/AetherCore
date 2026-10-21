#include <doctest/doctest.h>
#include "passes/FxaaDefs.hpp"

using namespace aether;

TEST_CASE("FXAA presentation scales only after resolving scene pixels")
{
	CHECK_FALSE(NeedsScaledPresentation({512, 512}, {512, 512}, false));
	CHECK(NeedsScaledPresentation({256, 256}, {512, 512}, false));
	CHECK(NeedsScaledPresentation({1024, 1024}, {512, 512}, false));
	CHECK(NeedsScaledPresentation({513, 255}, {1024, 512}, false));
	CHECK_FALSE(NeedsScaledPresentation({256, 256}, {512, 512}, true));
}
