#pragma once

// OverlayStyle.hpp — shared constants and drawing helpers for debug overlay panels.
// Both DebugLayer and SandboxLayer use these so the two panels look consistent.

#include <glm/glm.hpp>
#include <string_view>

#include "UiLayout.hpp"
#include "UIRenderer.hpp"

namespace aether::app::overlay
{
	// ── Colours ──────────────────────────────────────────────────────────────
	inline constexpr glm::vec4 kColorBg{ 0.07f, 0.09f, 0.12f, 0.91f };
	inline constexpr glm::vec4 kColorHeaderBg{ 0.10f, 0.13f, 0.17f, 0.97f };
	inline constexpr glm::vec4 kColorAccent{ 0.42f, 0.62f, 0.74f, 1.00f };
	inline constexpr glm::vec4 kColorSep{ 0.18f, 0.23f, 0.29f, 0.90f };
	inline constexpr glm::vec4 kColorText{ 0.88f, 0.91f, 0.93f, 1.00f };
	inline constexpr glm::vec4 kColorLabel{ 0.50f, 0.61f, 0.69f, 1.00f };
	inline constexpr glm::vec4 kColorSection{ 0.70f, 0.76f, 0.81f, 1.00f };
	inline constexpr glm::vec4 kColorTitle{ 0.90f, 0.88f, 0.82f, 1.00f };
	inline constexpr glm::vec4 kColorGood{ 0.40f, 0.72f, 0.46f, 0.95f };
	inline constexpr glm::vec4 kColorWarn{ 0.86f, 0.71f, 0.30f, 0.95f };
	inline constexpr glm::vec4 kColorBad{ 0.80f, 0.33f, 0.30f, 0.95f };

	// ── Layout constants ─────────────────────────────────────────────────────
	inline constexpr float kPad = 16.0f;
	inline constexpr float kRowH = 20.0f;  // vertical step between value rows
	inline constexpr float kHdrH = 48.0f;  // title block height
	inline constexpr float kCorner = 6.0f; // panel corner radius

	// ── Panel helpers ────────────────────────────────────────────────────────

	// Draws the outer panel background and coloured title-bar block.
	// panelL/R/top/bot are offsets from the given anchor.
	inline void DrawPanel(aether::UIRenderer& ui, glm::vec2 anchor, float panelL, float panelR, float panelTop, float panelBot)
	{
		ui.SetLayer(0);
		ui.DrawRect(
		        aether::UiRect{
		                .anchorMin = anchor,
		                .anchorMax = anchor,
		                .offsetMinPx = { panelL, panelTop },
		                .offsetMaxPx = { panelR, panelBot },
        },
		        kColorBg,
		        kCorner);

		ui.SetLayer(1);
		ui.DrawRect(
		        aether::UiRect{
		                .anchorMin = anchor,
		                .anchorMax = anchor,
		                .offsetMinPx = { panelL,         panelTop },
		                .offsetMaxPx = { panelR, panelTop + kHdrH },
        },
		        kColorHeaderBg,
		        kCorner);

		ui.SetLayer(2);
	}

	// Draws the panel title at the canonical vertical offset inside the header.
	inline void DrawTitle(aether::UIRenderer& ui, std::string_view text, glm::vec2 anchor, float panelL)
	{
		// kHdrH=48, title baseline at 26 gives ~10px top pad for 18pt glyphs
		ui.DrawText(text,
		        aether::UiPoint{
		                .anchor = anchor,
		                .offsetPx = { panelL + kPad, 26.0f },
        },
		        18.0f,
		        kColorTitle);
	}

	// Horizontal separator line.
	inline void DrawSeparator(aether::UIRenderer& ui, glm::vec2 anchor, float innerL, float innerR, float y)
	{
		ui.DrawLine(
		        aether::UiPoint{
		                .anchor = anchor, .offsetPx = { innerL, y }
        },
		        aether::UiPoint{ .anchor = anchor, .offsetPx = { innerR, y } },
		        1.0f,
		        kColorSep);
	}

	// Section header: accent bar + uppercase label.
	inline void DrawSectionHeader(aether::UIRenderer& ui, std::string_view label, glm::vec2 anchor, float panelL, float innerL, float y)
	{
		// 3 px accent bar aligned to the glyph cap-height
		ui.DrawRect(
		        aether::UiRect{
		                .anchorMin = anchor,
		                .anchorMax = anchor,
		                .offsetMinPx = { panelL + 2.f, y - 11.f },
		                .offsetMaxPx = { panelL + 5.f,  y + 2.f },
        },
		        kColorAccent);

		ui.DrawText(label,
		        aether::UiPoint{
		                .anchor = anchor, .offsetPx = { innerL, y }
        },
		        12.0f,
		        kColorSection);
	}

	// Two-column key / value row.
	inline void DrawKV(aether::UIRenderer& ui, std::string_view key, std::string_view val, glm::vec2 anchor, float xKey, float xVal, float y, glm::vec4 valColor = kColorText)
	{
		ui.DrawText(key,
		        aether::UiPoint{
		                .anchor = anchor, .offsetPx = { xKey, y }
        },
		        14.0f,
		        kColorLabel);
		ui.DrawText(val,
		        aether::UiPoint{
		                .anchor = anchor, .offsetPx = { xVal, y }
        },
		        14.0f,
		        valColor);
	}

	// Single full-width value row (no key label).
	inline void DrawValue(aether::UIRenderer& ui, std::string_view val, glm::vec2 anchor, float x, float y, glm::vec4 color = kColorText)
	{
		ui.DrawText(val,
		        aether::UiPoint{
		                .anchor = anchor, .offsetPx = { x, y }
        },
		        14.0f,
		        color);
	}

} // namespace aether::app::overlay
