#pragma once

#include <glm/vec4.hpp>

namespace aether::colors
{

	namespace detail
	{
		constexpr glm::vec4 rgb(int r, int g, int b) noexcept
		{
			return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
		}

		constexpr glm::vec4 rgba(int r, int g, int b, float a) noexcept
		{
			return {r / 255.0f, g / 255.0f, b / 255.0f, a};
		}
	} // namespace detail

	// ── AetherCore Warm Charcoal ─────────────────────────────────────────────────

	// Backgrounds
	inline constexpr auto Background = detail::rgb(30, 27, 24);      // #1E1B18  Warm dark charcoal
	inline constexpr auto Surface = detail::rgb(40, 36, 32);         // #282420  Panel
	inline constexpr auto SurfaceElevated = detail::rgb(51, 48, 44); // #33302C  Raised panel / modal
	inline constexpr auto Border = detail::rgb(74, 68, 62);          // #4A443E  Warm separator

	// Foreground
	inline constexpr auto TextPrimary = detail::rgb(226, 214, 196);   // #E2D6C4  Warm cream
	inline constexpr auto TextSecondary = detail::rgb(138, 126, 110); // #8A7E6E  Warm muted tan

	// Accents
	inline constexpr auto Orange = detail::rgb(255, 124, 50); // #FF7C32
	inline constexpr auto Yellow = detail::rgb(240, 188, 46); // #F0BC2E
	inline constexpr auto Green = detail::rgb(140, 216, 74);  // #8CD84A
	inline constexpr auto Red = detail::rgb(255, 64, 80);     // #FF4050

	// Semantic aliases
	inline constexpr auto Error = Red;
	inline constexpr auto Warn = Yellow;
	inline constexpr auto Success = Green;
	inline constexpr auto Primary = Orange;

	// Extended
	inline constexpr auto Info = detail::rgb(56, 189, 255);   // #38BDFF
	inline constexpr auto Mauve = detail::rgb(192, 128, 255); // #C080FF
	inline constexpr auto Neutral = detail::rgb(92, 82, 72);  // #5C5248  Warm mid-grey
	inline constexpr auto Overlay = detail::rgba(0, 0, 0, 0.6f);

	// Debug visualisation
	inline constexpr auto DebugRed = detail::rgb(255, 45, 85);     // #FF2D55
	inline constexpr auto DebugGreen = detail::rgb(50, 255, 126);  // #32FF7E
	inline constexpr auto DebugBlue = detail::rgb(41, 121, 255);   // #2979FF
	inline constexpr auto DebugYellow = detail::rgb(255, 211, 42); // #FFD32A
	inline constexpr auto DebugCyan = detail::rgb(23, 195, 224);   // #17C3E0

	// Frame time histogram gradient (fast → slow)
	inline constexpr auto HistFastest = detail::rgb(60, 200, 80);
	inline constexpr auto HistFast = detail::rgb(80, 200, 120);
	inline constexpr auto HistFair = detail::rgb(100, 200, 100);
	inline constexpr auto HistOkay = detail::rgb(160, 200, 80);
	inline constexpr auto HistSlow = detail::rgb(200, 180, 60);
	inline constexpr auto HistSlower = detail::rgb(200, 80, 60);
	inline constexpr auto HistBad = detail::rgb(160, 40, 40);
	inline constexpr auto HistTerrible = detail::rgb(120, 20, 20);

} // namespace aether::colors
