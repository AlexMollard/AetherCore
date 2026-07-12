#pragma once

#include <imgui.h>

#include "Color.hpp"

// The editor's shared chrome language, extracted from the project launcher
// (ProjectLauncherWindow defined it; panels adopting the look include this so
// the whole editor moves together). "Night Amber": warm near-black surfaces,
// hairline strokes, a single amber accent, spaced micro-labels, ghost/outline
// buttons, and corner brackets as the one overtly "gamer" flourish.
namespace aether::app::chrome
{
	[[nodiscard]] inline ImVec4 C(const glm::vec4& v)
	{
		return ImVec4{v.r, v.g, v.b, v.a};
	}

	// Palette tokens (engine/Color.hpp "Night Amber").
	inline const ImVec4 kBg = C(colors::Background);
	inline const ImVec4 kPanel = C(colors::Surface);
	inline const ImVec4 kPanelHi = C(colors::SurfaceElevated);
	inline const ImVec4 kStroke = C(colors::Border);
	inline const ImVec4 kAccent = C(colors::Primary);
	inline const ImVec4 kAccentHi = C(colors::PrimaryHover);
	inline const ImVec4 kAccentDim = C(colors::PrimaryActive);
	inline const ImVec4 kText = C(colors::TextPrimary);
	inline const ImVec4 kMuted = C(colors::TextSecondary);
	inline const ImVec4 kFaint = C(colors::TextFaint);
	inline const ImVec4 kOnAccent = C(colors::OnPrimary);

	[[nodiscard]] inline ImU32 U32(const ImVec4& color)
	{
		return ImGui::ColorConvertFloat4ToU32(color);
	}

	[[nodiscard]] inline ImVec4 WithAlpha(ImVec4 color, const float alpha)
	{
		color.w = alpha;
		return color;
	}

	// ── Interaction tokens ─────────────────────────────────────────────────────
	// Selection, hover, drop targets and drag ghosts all speak the single amber
	// accent so every panel matches (no per-panel blues/grays).
	inline const ImVec4 kSelectionBg = WithAlpha(kAccent, 0.28f);   // selected row fill
	inline const ImVec4 kSelectionBar = kAccentHi;                  // 3px leading bar on selection
	inline const ImVec4 kHoverBg = WithAlpha(kText, 0.06f);         // hovered row wash
	inline const ImVec4 kDropTarget = kAccentHi;                    // drop indicator lines / borders
	inline const ImVec4 kDropTargetBg = WithAlpha(kAccent, 0.20f);  // drop-into fill
	inline const ImVec4 kDragGhostBg = WithAlpha(kPanelHi, 0.94f);  // drag-payload card fill
	inline const ImVec4 kDragGhostBorder = WithAlpha(kAccent, 0.72f);
	inline const ImVec4 kSuccess = C(colors::Success);              // confirmation flashes / "ready"

	// Crisp arbitrary-size text (imgui 1.92 dynamic fonts bake per size).
	inline void TextSized(ImDrawList* drawList, const float size, const ImVec2 pos, const ImVec4& color, const char* text)
	{
		drawList->AddText(ImGui::GetFont(), size, pos, U32(color), text);
	}

	[[nodiscard]] inline ImVec2 MeasureSized(const float size, const char* text)
	{
		return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
	}

	// Targeting-reticle corner brackets - reserved for the focal element
	// (the launcher's hovered project row, the viewport's camera feed).
	inline void CornerBrackets(ImDrawList* drawList, const ImVec2 min, const ImVec2 max, const float arm, const float thickness, const ImVec4& color)
	{
		const ImU32 c = U32(color);
		drawList->AddLine(ImVec2(min.x, min.y), ImVec2(min.x + arm, min.y), c, thickness);
		drawList->AddLine(ImVec2(min.x, min.y), ImVec2(min.x, min.y + arm), c, thickness);
		drawList->AddLine(ImVec2(max.x - arm, min.y), ImVec2(max.x, min.y), c, thickness);
		drawList->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, min.y + arm), c, thickness);
		drawList->AddLine(ImVec2(min.x, max.y - arm), ImVec2(min.x, max.y), c, thickness);
		drawList->AddLine(ImVec2(min.x, max.y), ImVec2(min.x + arm, max.y), c, thickness);
		drawList->AddLine(ImVec2(max.x, max.y - arm), ImVec2(max.x, max.y), c, thickness);
		drawList->AddLine(ImVec2(max.x - arm, max.y), ImVec2(max.x, max.y), c, thickness);
	}

	// 2px accent hairline fading out toward both ends - the launcher's edge motif.
	inline void AccentHairline(ImDrawList* drawList, const ImVec2 min, const float width, const float alpha = 0.85f)
	{
		const float midX = min.x + width * 0.5f;
		const ImU32 solid = U32(WithAlpha(kAccent, alpha));
		const ImU32 clear = U32(WithAlpha(kAccent, 0.0f));
		drawList->AddRectFilledMultiColor(min, ImVec2(midX, min.y + 2.0f), clear, solid, solid, clear);
		drawList->AddRectFilledMultiColor(ImVec2(midX, min.y), ImVec2(min.x + width, min.y + 2.0f), solid, clear, clear, solid);
	}

	// Standard panel header band: amber tick + uppercase eyebrow + an optional
	// right-aligned micro-stat, over a fading hairline. Call first thing after
	// ImGui::Begin so every panel opens with the same composition.
	inline void PanelHeader(const char* eyebrow, const char* stat = nullptr)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float bandW = ImGui::GetContentRegionAvail().x;
		drawList->AddRectFilled(ImVec2(p.x, p.y + 1.0f), ImVec2(p.x + 3.0f, p.y + 13.0f), U32(kAccent));
		TextSized(drawList, 12.0f, ImVec2(p.x + 10.0f, p.y), kMuted, eyebrow);
		if (stat != nullptr && stat[0] != '\0')
		{
			const float statW = MeasureSized(12.0f, stat).x;
			TextSized(drawList, 12.0f, ImVec2(p.x + bandW - statW, p.y), kFaint, stat);
		}
		ImGui::Dummy(ImVec2(0.0f, 16.0f));
		AccentHairline(drawList, ImGui::GetCursorScreenPos(), bandW, 0.30f);
		ImGui::Dummy(ImVec2(0.0f, 4.0f));
	}

	// Micro section label: small amber tick + 13px uppercase muted text. Draws at
	// the current cursor and advances it (widget-flow friendly).
	inline void SectionTag(const char* label)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		drawList->AddRectFilled(ImVec2(pos.x, pos.y + 1.0f), ImVec2(pos.x + 3.0f, pos.y + 13.0f), U32(kAccent));
		TextSized(drawList, 13.0f, ImVec2(pos.x + 10.0f, pos.y), kMuted, label);
		ImGui::Dummy(ImVec2(10.0f + MeasureSized(13.0f, label).x, 15.0f));
	}

	// Amber-filled call-to-action (dark text).
	inline bool PrimaryButton(const char* label, const ImVec2 size = ImVec2(0.0f, 0.0f))
	{
		ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHi);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentDim);
		ImGui::PushStyleColor(ImGuiCol_Text, kOnAccent);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
		const bool pressed = ImGui::Button(label, size);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);
		return pressed;
	}

	// Amber outline, transparent fill - the secondary action.
	inline bool OutlineButton(const char* label, const ImVec2 size = ImVec2(0.0f, 0.0f))
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(kAccent, 0.14f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(kAccent, 0.22f));
		ImGui::PushStyleColor(ImGuiCol_Text, kAccentHi);
		ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(kAccent, 0.55f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		const bool pressed = ImGui::Button(label, size);
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(5);
		return pressed;
	}

	// Quiet control: transparent until hovered (amber wash), muted text.
	inline bool GhostButton(const char* label, const ImVec2 size = ImVec2(0.0f, 0.0f), const ImVec4& textColor = kMuted)
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(kAccent, 0.12f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(kAccent, 0.20f));
		ImGui::PushStyleColor(ImGuiCol_Text, textColor);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
		const bool pressed = ImGui::Button(label, size);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);
		return pressed;
	}

	// Selected state for a segmented control / tool button: amber fill.
	inline bool ActiveToolButton(const char* label, const ImVec2 size)
	{
		return PrimaryButton(label, size);
	}
} // namespace aether::app::chrome
