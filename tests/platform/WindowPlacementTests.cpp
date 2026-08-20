// The window a published game opens. Two mistakes have already been made in this
// arithmetic, so it lives in a pure function that can be checked without a display:
//   - measuring the CLIENT area against a work area that has to hold the whole window,
//     which let a window "fit" while its bottom edge sat off-screen;
//   - not shrinking at all, so the shipped 2560x1440 default opened a window larger than
//     a 1080p screen with the title bar above the top of it.
#include <doctest/doctest.h>

#include "platform/WindowPlacement.hpp"

using aether::FitWindowToWorkArea;
using aether::WindowFit;

namespace
{
	// A typical Windows frame: a thin border either side, a title bar on top.
	constexpr int kChromeW = 16;
	constexpr int kChromeH = 39;
} // namespace

TEST_CASE("A window that fits is left at the size it asked for")
{
	const WindowFit fit = FitWindowToWorkArea(1280, 720, kChromeW, kChromeH, 0, 0, 1920, 1040, 960, 520);
	CHECK_FALSE(fit.resized);
	CHECK(fit.clientWidth == 1280);
	CHECK(fit.clientHeight == 720);
}

TEST_CASE("The shipped default is fitted onto a smaller display")
{
	// 2560x1440 is what a published game asks for out of the box. On 1080p it has to come
	// down, chrome included, or the title bar lands off the top of the screen.
	const WindowFit fit = FitWindowToWorkArea(2560, 1440, kChromeW, kChromeH, 0, 0, 1920, 1040, 960, 520);
	REQUIRE(fit.resized);
	CHECK(fit.clientWidth == 1920 - kChromeW);
	CHECK(fit.clientHeight == 1040 - kChromeH);

	// The whole window, not just the client area, is inside the work area.
	CHECK(fit.outerX >= 0);
	CHECK(fit.outerY >= 0);
	CHECK(fit.outerX + fit.clientWidth + kChromeW <= 1920);
	CHECK(fit.outerY + fit.clientHeight + kChromeH <= 1040);
}

TEST_CASE("Chrome counts towards the fit")
{
	// A client area exactly the width of the work area does NOT fit: the borders still
	// have to go somewhere. This is the bug that made a window look placed while its
	// bottom edge, and the status bar with it, was off-screen.
	const WindowFit fit = FitWindowToWorkArea(1920, 1040, kChromeW, kChromeH, 0, 0, 1920, 1040, 960, 520);
	REQUIRE(fit.resized);
	CHECK(fit.clientWidth == 1920 - kChromeW);
	CHECK(fit.clientHeight == 1040 - kChromeH);
}

TEST_CASE("A fitting window is centred on the point it was given")
{
	const WindowFit fit = FitWindowToWorkArea(800, 600, kChromeW, kChromeH, 0, 0, 1920, 1040, 960, 520);
	CHECK_FALSE(fit.resized);
	CHECK(fit.outerX == 960 - (800 + kChromeW) / 2);
	CHECK(fit.outerY == 520 - (600 + kChromeH) / 2);
}

TEST_CASE("A point near an edge pulls the window back inside instead of hanging off")
{
	const WindowFit fit = FitWindowToWorkArea(800, 600, kChromeW, kChromeH, 0, 0, 1920, 1040, 1900, 1030);
	CHECK(fit.outerX + 800 + kChromeW <= 1920);
	CHECK(fit.outerY + 600 + kChromeH <= 1040);
}

TEST_CASE("A work area that does not start at the origin is respected")
{
	// A second monitor to the left of the primary, and a taskbar down the side.
	constexpr int kAreaX = -1920;
	constexpr int kAreaY = 40;
	const WindowFit fit = FitWindowToWorkArea(3000, 2000, kChromeW, kChromeH, kAreaX, kAreaY, 1920, 1000, kAreaX + 960, kAreaY + 500);
	REQUIRE(fit.resized);
	CHECK(fit.outerX >= kAreaX);
	CHECK(fit.outerY >= kAreaY);
	CHECK(fit.outerX + fit.clientWidth + kChromeW <= kAreaX + 1920);
	CHECK(fit.outerY + fit.clientHeight + kChromeH <= kAreaY + 1000);
}

TEST_CASE("A work area too small for any window leaves the size alone")
{
	// Rather than asking for a zero or negative size, which is not a window at all.
	const WindowFit fit = FitWindowToWorkArea(800, 600, kChromeW, kChromeH, 0, 0, 8, 8, 4, 4);
	CHECK_FALSE(fit.resized);
	CHECK(fit.clientWidth == 800);
	CHECK(fit.clientHeight == 600);
	// The clamp range must not invert when the window cannot fit.
	CHECK(fit.outerX == 0);
	CHECK(fit.outerY == 0);
}
