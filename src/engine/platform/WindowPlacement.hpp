#pragma once

#include <algorithm>

namespace aether
{
	// Where a windowed window should sit, given what it asked for and what the display can
	// actually show. Split out of Window so the arithmetic can be tested without a display -
	// it is the part that has been wrong twice: once measuring the CLIENT area against a
	// work area that has to hold the whole window, and once not shrinking at all, which is
	// how a published game asking for the shipped 2560x1440 default opened a window larger
	// than a 1080p screen with its title bar off the top.
	struct WindowFit
	{
		int clientWidth = 0;
		int clientHeight = 0;
		// Top-left of the WHOLE window, chrome included. Callers that position the client
		// area (glfwSetWindowPos does) add the left/top frame thickness back on.
		int outerX = 0;
		int outerY = 0;
		bool resized = false;
	};

	// chromeW/chromeH are the total border+title thickness, so outer = client + chrome.
	// A work area too small to hold any window at all leaves the size alone rather than
	// asking for a zero-sized one.
	[[nodiscard]] inline WindowFit FitWindowToWorkArea(const int clientWidth, const int clientHeight, const int chromeW, const int chromeH, const int areaX, const int areaY, const int areaW, const int areaH,
	        const int centerX, const int centerY)
	{
		WindowFit fit;
		fit.clientWidth = clientWidth;
		fit.clientHeight = clientHeight;

		if (clientWidth + chromeW > areaW || clientHeight + chromeH > areaH)
		{
			const int fittedWidth = std::min(clientWidth, areaW - chromeW);
			const int fittedHeight = std::min(clientHeight, areaH - chromeH);
			if (fittedWidth > 0 && fittedHeight > 0)
			{
				fit.clientWidth = fittedWidth;
				fit.clientHeight = fittedHeight;
				fit.resized = true;
			}
		}

		const int outerW = fit.clientWidth + chromeW;
		const int outerH = fit.clientHeight + chromeH;
		// Centre on the point, then pull back inside the area. std::max guards the case
		// where the window still does not fit (a work area smaller than the chrome), so the
		// clamp range never inverts.
		fit.outerX = std::clamp(centerX - outerW / 2, areaX, std::max(areaX, areaX + areaW - outerW));
		fit.outerY = std::clamp(centerY - outerH / 2, areaY, std::max(areaY, areaY + areaH - outerH));
		return fit;
	}
} // namespace aether
