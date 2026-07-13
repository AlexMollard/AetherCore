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

	// ── AetherCore Night Amber ───────────────────────────────────────────────────
	// Warm near-black neutrals with a single amber accent - the language set by
	// the project launcher. One accent; warm grays carry everything else.

	// Backgrounds
	inline constexpr auto Background = detail::rgb(15, 13, 11);      // #0F0D0B  Near-black, warm-leaning
	inline constexpr auto Surface = detail::rgb(25, 22, 19);         // #191613  Panel
	inline constexpr auto SurfaceElevated = detail::rgb(32, 28, 24); // #201C18  Raised panel / modal
	inline constexpr auto Border = detail::rgb(50, 44, 38);          // #322C26  Hairline separator

	// Foreground
	inline constexpr auto TextPrimary = detail::rgb(237, 233, 227);   // #EDE9E3  Warm near-white
	inline constexpr auto TextSecondary = detail::rgb(150, 141, 128); // #968D80  Warm muted gray
	inline constexpr auto TextFaint = detail::rgb(99, 91, 80);        // #635B50  De-emphasized / hints

	// Accents
	inline constexpr auto Orange = detail::rgb(255, 148, 41); // #FF9429  The accent
	inline constexpr auto Yellow = detail::rgb(240, 188, 46); // #F0BC2E
	inline constexpr auto Green = detail::rgb(140, 216, 74);  // #8CD84A
	inline constexpr auto Red = detail::rgb(255, 64, 80);     // #FF4050

	// Primary interaction states + on-accent text (dark text on amber fills).
	inline constexpr auto PrimaryHover = detail::rgb(255, 179, 82);  // #FFB352
	inline constexpr auto PrimaryActive = detail::rgb(184, 107, 31); // #B86B1F
	inline constexpr auto OnPrimary = detail::rgb(18, 15, 11);       // #120F0B

	// Semantic aliases
	inline constexpr auto Error = Red;
	inline constexpr auto Warn = Yellow;
	inline constexpr auto Success = Green;
	inline constexpr auto Primary = Orange;

	// Extended
	inline constexpr auto Info = detail::rgb(56, 189, 255);   // #38BDFF
	inline constexpr auto Mauve = detail::rgb(192, 128, 255); // #C080FF
	inline constexpr auto Neutral = detail::rgb(105, 96, 86); // #696056  Warm mid-grey
	inline constexpr auto Overlay = detail::rgba(0, 0, 0, 0.6f);

	// Spatial axes (X/Y/Z) - one convention shared by the move/rotate/scale gizmos
	// and the inspector's Vec3 rows so a red field always means X, etc. Tuned to
	// the Night Amber world: hue identity kept (red/green/blue muscle memory) but
	// warmed + desaturated so they sit in the warm near-black palette instead of
	// reading as generic editor primaries.
	inline constexpr auto AxisX = detail::rgb(198, 91, 76);   // #C65B4C  clay rust
	inline constexpr auto AxisY = detail::rgb(150, 172, 88);  // #96AC58  olive moss
	inline constexpr auto AxisZ = detail::rgb(108, 141, 181); // #6C8DB5  dusty steel

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
