#pragma once

// OverlayStyle.hpp - shared constants, drawing helpers, and PanelBuilder for debug overlay panels.

#include <array>
#include <initializer_list>
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
	inline constexpr float kPad = 16.0f;   // inner padding from panel edges
	inline constexpr float kRowH = 20.0f;  // vertical step between value rows
	inline constexpr float kHdrH = 48.0f;  // title block height
	inline constexpr float kCorner = 6.0f; // panel corner radius

	// ── Low-level panel helpers ──────────────────────────────────────────────
	// These remain available for callers with custom layouts (e.g. DebugLayer graph).

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

	inline void DrawSectionHeader(aether::UIRenderer& ui, std::string_view label, glm::vec2 anchor, float panelL, float innerL, float y)
	{
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

	inline void DrawValue(aether::UIRenderer& ui, std::string_view val, glm::vec2 anchor, float x, float y, glm::vec4 color = kColorText)
	{
		ui.DrawText(val,
		        aether::UiPoint{
		                .anchor = anchor, .offsetPx = { x, y }
        },
		        14.0f,
		        color);
	}

	// ── PanelBuilder ─────────────────────────────────────────────────────────
	// RAII helper that tracks cursor Y and draws the background panel on destruction
	// with the correct auto-computed height - no more hand-tuned kPanelBot constants.
	//
	// Single-column (val offset relative to inner-left edge):
	//   PanelBuilder panel(ui, anchor, panelL, panelR, panelTop, valColOffset);
	//   panel.Title("MY PANEL").Section("DATA");
	//   panel.KV("Key", "Value");
	//   panel.KV("Status", "OK", kColorGood);
	//   // background auto-drawn when panel goes out of scope
	//
	// Multi-column (explicit key/val x offsets in the same coordinate space as panelL):
	//   PanelBuilder panel(ui, anchor, panelL, panelR, panelTop,
	//                      { {keyX0, valX0}, {keyX1, valX1} });
	//   panel.Title("MY PANEL").Section("SECTION");
	//   panel.KVCol(0, "Left Key",  "Left Val");
	//   panel.KVCol(1, "Right Key", "Right Val", kColorWarn);
	//   panel.NextRow();
	//
	// Custom content (graph, free text, etc.):
	//   panel.Custom(72.f, [&](float y) { DrawMyGraph(ui, panel.InnerL(), y); });
	//   panel.Skip(8.f);  // extra vertical breathing room
	class PanelBuilder
	{
	public:
		struct ColumnDef
		{
			float keyX = 0.f; // x offset from anchor (same coordinate space as panelL)
			float valX = 0.f;
		};

		// Single-column: val positioned at innerL + valColOffset.
		PanelBuilder(aether::UIRenderer& ui, glm::vec2 anchor, float panelL, float panelR, float panelTop, float valColOffset = 110.f)
		      : m_ui(ui), m_anchor(anchor), m_panelL(panelL), m_panelR(panelR), m_panelTop(panelTop), m_innerL(panelL + kPad), m_innerR(panelR - kPad), m_y(panelTop)
		{
			const float il = panelL + kPad;
			m_columns[0] = { il, il + valColOffset };
			m_columnCount = 1;
			ui.SetLayer(2);
		}

		// Multi-column: explicit key/val x offsets from anchor (up to 4 columns).
		PanelBuilder(aether::UIRenderer& ui, glm::vec2 anchor, float panelL, float panelR, float panelTop, std::initializer_list<ColumnDef> columns)
		      : m_ui(ui), m_anchor(anchor), m_panelL(panelL), m_panelR(panelR), m_panelTop(panelTop), m_innerL(panelL + kPad), m_innerR(panelR - kPad), m_y(panelTop)
		{
			m_columnCount = 0;
			for (const ColumnDef& c: columns)
			{
				if (m_columnCount < static_cast<int>(m_columns.size()))
				{
					m_columns[m_columnCount++] = c;
				}
			}
			ui.SetLayer(2);
		}

		// Background is drawn on destruction - content layers are already queued with
		// layer 2; DrawPanel submits layers 0+1 which the renderer sorts to the back.
		~PanelBuilder()
		{
			Finish();
		}

		PanelBuilder(const PanelBuilder&) = delete;
		PanelBuilder& operator=(const PanelBuilder&) = delete;

		// Draws the title text and first separator; advances cursor to first section y.
		PanelBuilder& Title(std::string_view text)
		{
			m_ui.DrawText(text,
			        aether::UiPoint{
			                .anchor = m_anchor, .offsetPx = { m_innerL, m_panelTop + 26.f }
            },
			        18.f,
			        kColorTitle);
			DrawSeparator(m_ui, m_anchor, m_innerL, m_innerR, m_panelTop + 54.f);
			m_y = m_panelTop + 66.f;
			return *this;
		}

		// Draws a section header and separator.
		// Adds a kRowH inter-section gap for every section after the first.
		PanelBuilder& Section(std::string_view label)
		{
			if (!m_firstSection)
			{
				m_y += kRowH;
			}
			m_firstSection = false;
			DrawSectionHeader(m_ui, label, m_anchor, m_panelL, m_innerL, m_y);
			DrawSeparator(m_ui, m_anchor, m_innerL, m_innerR, m_y + 13.f);
			m_y += 34.f;
			return *this;
		}

		// Draws a key-value row in column 0 then advances to the next row.
		PanelBuilder& KV(std::string_view key, std::string_view val, glm::vec4 valColor = kColorText)
		{
			if (m_columnCount > 0)
			{
				DrawKV(m_ui, key, val, m_anchor, m_columns[0].keyX, m_columns[0].valX, m_y, valColor);
			}
			m_y += kRowH;
			return *this;
		}

		// Draws a key-value row in the specified column WITHOUT advancing the cursor.
		// Call NextRow() after all columns for this row are filled.
		PanelBuilder& KVCol(int col, std::string_view key, std::string_view val, glm::vec4 valColor = kColorText)
		{
			if (col >= 0 && col < m_columnCount)
			{
				DrawKV(m_ui, key, val, m_anchor, m_columns[col].keyX, m_columns[col].valX, m_y, valColor);
			}
			return *this;
		}

		// Draws a full-width value using column 0's key-x and advances to the next row.
		PanelBuilder& Value(std::string_view val, glm::vec4 color = kColorText)
		{
			if (m_columnCount > 0)
			{
				DrawValue(m_ui, val, m_anchor, m_columns[0].keyX, m_y, color);
			}
			m_y += kRowH;
			return *this;
		}

		// Advances to the next row after a set of KVCol calls.
		PanelBuilder& NextRow()
		{
			m_y += kRowH;
			return *this;
		}

		// Advances the cursor by px without drawing anything.
		PanelBuilder& Skip(float px = kRowH)
		{
			m_y += px;
			return *this;
		}

		// Executes a custom draw function at the current cursor Y then advances by height.
		// Useful for graphs, progress bars, or any free-form content.
		// The callback signature is: void(float cursorY)
		template<typename F>
		PanelBuilder& Custom(float height, F&& draw)
		{
			std::forward<F>(draw)(m_y);
			m_y += height;
			return *this;
		}

		// Accessors for callers that need to issue manual draw calls.
		float CurrentY() const
		{
			return m_y;
		}

		glm::vec2 Anchor() const
		{
			return m_anchor;
		}

		float InnerL() const
		{
			return m_innerL;
		}

		float InnerR() const
		{
			return m_innerR;
		}

		float PanelL() const
		{
			return m_panelL;
		}

		float PanelR() const
		{
			return m_panelR;
		}

	private:
		void Finish()
		{
			if (m_finished)
			{
				return;
			}
			m_finished = true;
			DrawPanel(m_ui, m_anchor, m_panelL, m_panelR, m_panelTop, m_y + kPad);
		}

		aether::UIRenderer& m_ui;
		glm::vec2 m_anchor;
		float m_panelL, m_panelR, m_panelTop;
		float m_innerL, m_innerR;
		std::array<ColumnDef, 4> m_columns{};
		int m_columnCount = 0;
		float m_y = 0.f;
		bool m_firstSection = true;
		bool m_finished = false;
	};

} // namespace aether::app::overlay
