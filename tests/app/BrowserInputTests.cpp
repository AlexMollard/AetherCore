#include <doctest/doctest.h>
#include "platform/web/BrowserKey.hpp"
#include "platform/Input.hpp"

namespace
{
	template <typename T>
	void CheckBrowserTickDeltas(T& input)
	{
		if constexpr (requires { input.SetBrowserMouseDelta(glm::vec2{}); input.SetBrowserScrollDelta(glm::vec2{}); input.ClearBrowserDeltas(); })
		{
			input.SetBrowserMouseDelta({12.f, -4.f});
			input.SetBrowserScrollDelta({0.f, 2.f});
			CHECK(input.GetMouseDelta() == glm::vec2(12.f, -4.f));
			CHECK(input.GetScrollDelta() == glm::vec2(0.f, 2.f));
			// All scripts on the first tick see the same sample, but catch-up ticks do not replay it.
			CHECK(input.GetMouseDelta() == glm::vec2(12.f, -4.f));
			input.ClearBrowserDeltas();
			for (int tick = 0; tick < 2; ++tick)
			{
				CHECK(input.GetMouseDelta() == glm::vec2{});
				CHECK(input.GetScrollDelta() == glm::vec2{});
				input.ClearBrowserDeltas();
			}
		}
		else
		{
			FAIL_CHECK("Browser input must expose and clear a gameplay tick sample independently of pointer lock");
		}
	}
}

TEST_CASE("Browser pointer and scroll deltas are consumed once across catch-up ticks")
{
	aether::Input input;
	SUBCASE("Unlocked pointer") { CHECK_FALSE(input.IsCursorLocked()); }
	SUBCASE("Locked pointer")
	{
		input.RequestCursorLock(true);
		input.UpdateCursorLock(true);
		REQUIRE(input.IsCursorLocked());
	}
	CheckBrowserTickDeltas(input);
}

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
