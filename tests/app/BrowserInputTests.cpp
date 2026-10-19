#include <doctest/doctest.h>
#include "platform/web/BrowserKey.hpp"

TEST_CASE("Browser physical keys map to the shared input codes")
{
	CHECK(aether::web::BrowserKey("KeyA") == 65);
	CHECK(aether::web::BrowserKey("KeyZ") == 90);
	CHECK(aether::web::BrowserKey("Digit0") == 48);
	CHECK(aether::web::BrowserKey("ArrowDown") == 264);
	CHECK(aether::web::BrowserKey("Escape") == 256);
	CHECK(aether::web::BrowserKey("F12") == 301);
	CHECK(aether::web::BrowserKey("Numpad7") == 327);
	CHECK(aether::web::BrowserKey("ShiftRight") == 344);
	CHECK(aether::web::BrowserKey("Space") == 32);
	CHECK(aether::web::BrowserKey("Unidentified") == -1);
	CHECK(aether::web::BrowserKey("Key[") == -1);
}
