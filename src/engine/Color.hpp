#pragma once

#include <glm/vec4.hpp>

namespace aether::colors
{

	namespace detail
	{
		constexpr glm::vec4 rgb(int r, int g, int b) noexcept
		{
			return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, 1.0f};
		}

		constexpr glm::vec4 rgba(int r, int g, int b, float a) noexcept
		{
			return {static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, a};
		}
	} // namespace detail

	inline constexpr auto Background = detail::rgb(15, 13, 11);
	inline constexpr auto Surface = detail::rgb(25, 22, 19);
	inline constexpr auto SurfaceElevated = detail::rgb(32, 28, 24);
	inline constexpr auto Border = detail::rgb(50, 44, 38);

	inline constexpr auto TextPrimary = detail::rgb(237, 233, 227);
	inline constexpr auto TextSecondary = detail::rgb(150, 141, 128);
	inline constexpr auto TextFaint = detail::rgb(99, 91, 80);

	inline constexpr auto Orange = detail::rgb(255, 148, 41);
	inline constexpr auto Yellow = detail::rgb(240, 188, 46);
	inline constexpr auto Green = detail::rgb(140, 216, 74);
	inline constexpr auto Red = detail::rgb(255, 64, 80);

	inline constexpr auto PrimaryHover = detail::rgb(255, 179, 82);
	inline constexpr auto PrimaryActive = detail::rgb(184, 107, 31);
	inline constexpr auto OnPrimary = detail::rgb(18, 15, 11);

	inline constexpr auto Error = Red;
	inline constexpr auto Warn = Yellow;
	inline constexpr auto Success = Green;
	inline constexpr auto Primary = Orange;

	inline constexpr auto Info = detail::rgb(56, 189, 255);
	inline constexpr auto Mauve = detail::rgb(192, 128, 255);
	inline constexpr auto Neutral = detail::rgb(105, 96, 86);
	inline constexpr auto Overlay = detail::rgba(0, 0, 0, 0.6f);

	inline constexpr auto AxisX = detail::rgb(198, 91, 76);
	inline constexpr auto AxisY = detail::rgb(150, 172, 88);
	inline constexpr auto AxisZ = detail::rgb(108, 141, 181);

	inline constexpr auto DebugRed = detail::rgb(255, 45, 85);
	inline constexpr auto DebugGreen = detail::rgb(50, 255, 126);
	inline constexpr auto DebugBlue = detail::rgb(41, 121, 255);
	inline constexpr auto DebugYellow = detail::rgb(255, 211, 42);
	inline constexpr auto DebugCyan = detail::rgb(23, 195, 224);

	inline constexpr auto HistFastest = detail::rgb(60, 200, 80);
	inline constexpr auto HistFast = detail::rgb(80, 200, 120);
	inline constexpr auto HistFair = detail::rgb(100, 200, 100);
	inline constexpr auto HistOkay = detail::rgb(160, 200, 80);
	inline constexpr auto HistSlow = detail::rgb(200, 180, 60);
	inline constexpr auto HistSlower = detail::rgb(200, 80, 60);
	inline constexpr auto HistBad = detail::rgb(160, 40, 40);
	inline constexpr auto HistTerrible = detail::rgb(120, 20, 20);

} // namespace aether::colors
