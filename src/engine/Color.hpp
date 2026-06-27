#pragma once

#include <glm/vec4.hpp>

namespace aether::colors
{

	// ── Gruvbox Pastel palette ─────────────────────────────────────────────────

	// Backgrounds
	inline constexpr auto Background = glm::vec4(42.0f / 255.0f, 40.0f / 255.0f, 39.0f / 255.0f, 1.0f); // #2A2827 Dark Cream
	inline constexpr auto Surface = glm::vec4(60.0f / 255.0f, 56.0f / 255.0f, 54.0f / 255.0f, 1.0f);    // #3C3836 Warm Charcoal

	// Foreground
	inline constexpr auto TextPrimary = glm::vec4(251.0f / 255.0f, 241.0f / 255.0f, 199.0f / 255.0f, 1.0f);   // #FBF1C7 Soft Ivory
	inline constexpr auto TextSecondary = glm::vec4(189.0f / 255.0f, 174.0f / 255.0f, 147.0f / 255.0f, 1.0f); // #BDAE93 Muted Almond

	// Accents
	inline constexpr auto Orange = glm::vec4(254.0f / 255.0f, 128.0f / 255.0f, 25.0f / 255.0f, 1.0f); // #FE8019 Pastel Orange
	inline constexpr auto Yellow = glm::vec4(250.0f / 255.0f, 189.0f / 255.0f, 47.0f / 255.0f, 1.0f); // #FABD2F Soft Gold
	inline constexpr auto Green = glm::vec4(184.0f / 255.0f, 187.0f / 255.0f, 38.0f / 255.0f, 1.0f);  // #B8BB26 Sage Green
	inline constexpr auto Red = glm::vec4(251.0f / 255.0f, 73.0f / 255.0f, 52.0f / 255.0f, 1.0f);     // #FB4934 Dusty Coral

	// Semantic aliases
	inline constexpr auto Error = Red;
	inline constexpr auto Warn = Yellow;
	inline constexpr auto Success = Green;
	inline constexpr auto Primary = Orange;

	// Extended colours
	inline constexpr auto Info = glm::vec4(131.0f / 255.0f, 165.0f / 255.0f, 152.0f / 255.0f, 1.0f);    // #83A598 Muted Teal
	inline constexpr auto Mauve = glm::vec4(211.0f / 255.0f, 134.0f / 255.0f, 155.0f / 255.0f, 1.0f);   // #D3869B Soft Mauve
	inline constexpr auto Neutral = glm::vec4(146.0f / 255.0f, 131.0f / 255.0f, 116.0f / 255.0f, 1.0f); // #928374 Warm Grey

	// Debug-visualisation colours (distinct from semantic accents above)
	inline constexpr auto DebugRed = glm::vec4(204.0f / 255.0f, 36.0f / 255.0f, 29.0f / 255.0f, 1.0f);     // #CC241D Brick Red
	inline constexpr auto DebugGreen = glm::vec4(152.0f / 255.0f, 151.0f / 255.0f, 26.0f / 255.0f, 1.0f);  // #98971A Olive Green
	inline constexpr auto DebugBlue = glm::vec4(95.0f / 255.0f, 139.0f / 255.0f, 176.0f / 255.0f, 1.0f);   // #5F8BB0 Dusty Slate Blue
	inline constexpr auto DebugYellow = glm::vec4(215.0f / 255.0f, 153.0f / 255.0f, 33.0f / 255.0f, 1.0f); // #D79921 Amber
	inline constexpr auto DebugCyan = glm::vec4(104.0f / 255.0f, 154.0f / 255.0f, 106.0f / 255.0f, 1.0f);  // #689D6A Muted Aqua

} // namespace aether::colors
