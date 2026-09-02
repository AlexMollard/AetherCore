#pragma once

#include <cmath>

#include <imgui.h>

#include "Color.hpp"

namespace aether::editor::chrome
{
	// What a button of this label will measure. Toolbars that guessed the width with a round
	// number left their last control hanging off the panel edge.
	[[nodiscard]] inline float ButtonWidth(const char* label)
	{
		return ImGui::CalcTextSize(label, nullptr, /*hide_text_after_double_hash=*/true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
	}

	// SameLine, unless the next control of this width would not fit - then start a row. A
	// toolbar built from plain SameLine calls simply runs off the right edge once the panel
	// is narrow enough, and nothing on screen says the control is there.
	inline void SameLineOrWrap(float nextWidth, float spacing = -1.0f)
	{
		ImGui::SameLine(0.0f, spacing);
		if (ImGui::GetContentRegionAvail().x < nextWidth)
		{
			ImGui::NewLine();
		}
	}

	[[nodiscard]] inline ImVec4 C(const glm::vec4& v)
	{
		return ImVec4{v.r, v.g, v.b, v.a};
	}

	[[nodiscard]] inline ImU32 U32(const ImVec4& color)
	{
		return ImGui::ColorConvertFloat4ToU32(color);
	}

	[[nodiscard]] inline ImVec4 WithAlpha(ImVec4 color, const float alpha)
	{
		color.w = alpha;
		return color;
	}

	struct EditorTheme
	{
		ImVec4 background;
		ImVec4 surface;
		ImVec4 surfaceElevated;
		ImVec4 border;
		ImVec4 accent;
		ImVec4 accentHover;
		ImVec4 accentActive;
		ImVec4 onAccent;
		ImVec4 textPrimary;
		ImVec4 textSecondary;
		ImVec4 textFaint;
		ImVec4 success;
		ImVec4 warning;
		ImVec4 error;
	};

	[[nodiscard]] inline EditorTheme NightAmberTheme()
	{
		return EditorTheme{
		        .background = C(colors::Background),
		        .surface = C(colors::Surface),
		        .surfaceElevated = C(colors::SurfaceElevated),
		        .border = C(colors::Border),
		        .accent = C(colors::Primary),
		        .accentHover = C(colors::PrimaryHover),
		        .accentActive = C(colors::PrimaryActive),
		        .onAccent = C(colors::OnPrimary),
		        .textPrimary = C(colors::TextPrimary),
		        .textSecondary = C(colors::TextSecondary),
		        .textFaint = C(colors::TextFaint),
		        .success = C(colors::Success),
		        .warning = C(colors::Warn),
		        .error = C(colors::Error),
		};
	}

	[[nodiscard]] inline EditorTheme& ActiveTheme()
	{
		static EditorTheme theme = NightAmberTheme();
		return theme;
	}

	inline ImVec4 kBg = C(colors::Background);
	inline ImVec4 kPanel = C(colors::Surface);
	inline ImVec4 kPanelHi = C(colors::SurfaceElevated);
	inline ImVec4 kStroke = C(colors::Border);
	inline ImVec4 kAccent = C(colors::Primary);
	inline ImVec4 kAccentHi = C(colors::PrimaryHover);
	inline ImVec4 kAccentDim = C(colors::PrimaryActive);
	inline ImVec4 kText = C(colors::TextPrimary);
	inline ImVec4 kMuted = C(colors::TextSecondary);
	inline ImVec4 kFaint = C(colors::TextFaint);
	inline ImVec4 kOnAccent = C(colors::OnPrimary);
	inline ImVec4 kSuccess = C(colors::Success);
	inline ImVec4 kWarning = C(colors::Warn);
	inline ImVec4 kError = C(colors::Error);

	inline ImVec4 kSelectionBg = WithAlpha(C(colors::Primary), 0.28f);
	inline ImVec4 kSelectionBar = C(colors::PrimaryHover);
	inline ImVec4 kHoverBg = WithAlpha(C(colors::TextPrimary), 0.06f);
	inline ImVec4 kDropTarget = C(colors::PrimaryHover);
	inline ImVec4 kDropTargetBg = WithAlpha(C(colors::Primary), 0.20f);
	inline ImVec4 kDragGhostBg = WithAlpha(C(colors::SurfaceElevated), 0.94f);
	inline ImVec4 kDragGhostBorder = WithAlpha(C(colors::Primary), 0.72f);

	inline void RefreshTokens()
	{
		const EditorTheme& t = ActiveTheme();
		kBg = t.background;
		kPanel = t.surface;
		kPanelHi = t.surfaceElevated;
		kStroke = t.border;
		kAccent = t.accent;
		kAccentHi = t.accentHover;
		kAccentDim = t.accentActive;
		kText = t.textPrimary;
		kMuted = t.textSecondary;
		kFaint = t.textFaint;
		kOnAccent = t.onAccent;
		kSuccess = t.success;
		kWarning = t.warning;
		kError = t.error;
		kSelectionBg = WithAlpha(kAccent, 0.28f);
		kSelectionBar = kAccentHi;
		kHoverBg = WithAlpha(kText, 0.06f);
		kDropTarget = kAccentHi;
		kDropTargetBg = WithAlpha(kAccent, 0.20f);
		kDragGhostBg = WithAlpha(kPanelHi, 0.94f);
		kDragGhostBorder = WithAlpha(kAccent, 0.72f);
	}

	inline void ApplyImGuiColors(ImGuiStyle& style)
	{
		const EditorTheme& t = ActiveTheme();
		ImVec4* c = style.Colors;
		const auto A = [](const ImVec4& col, const float a)
		{
			return ImVec4(col.x, col.y, col.z, a);
		};

		c[ImGuiCol_Text] = t.textPrimary;
		c[ImGuiCol_TextDisabled] = t.textSecondary;
		c[ImGuiCol_TextLink] = t.accent;
		c[ImGuiCol_TextSelectedBg] = A(t.accent, 0.19f);

		c[ImGuiCol_WindowBg] = t.background;
		c[ImGuiCol_ChildBg] = t.background;
		c[ImGuiCol_PopupBg] = t.surfaceElevated;
		c[ImGuiCol_Border] = t.border;
		c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

		c[ImGuiCol_TitleBg] = t.surface;
		c[ImGuiCol_TitleBgActive] = t.surfaceElevated;
		c[ImGuiCol_TitleBgCollapsed] = t.background;
		c[ImGuiCol_MenuBarBg] = t.surface;

		c[ImGuiCol_ScrollbarBg] = t.background;
		c[ImGuiCol_ScrollbarGrab] = t.surfaceElevated;
		c[ImGuiCol_ScrollbarGrabHovered] = A(t.accent, 0.55f);
		c[ImGuiCol_ScrollbarGrabActive] = t.accent;

		c[ImGuiCol_CheckMark] = t.accent;
		c[ImGuiCol_CheckboxSelectedBg] = A(t.accent, 0.16f);
		c[ImGuiCol_SliderGrab] = t.accent;
		c[ImGuiCol_SliderGrabActive] = t.accentHover;

		c[ImGuiCol_Button] = t.surface;
		c[ImGuiCol_ButtonHovered] = A(t.accent, 0.70f);
		c[ImGuiCol_ButtonActive] = t.accent;

		c[ImGuiCol_Header] = t.surface;
		c[ImGuiCol_HeaderHovered] = A(t.accent, 0.39f);
		c[ImGuiCol_HeaderActive] = A(t.accent, 0.63f);

		c[ImGuiCol_Separator] = t.border;
		c[ImGuiCol_SeparatorHovered] = t.accent;
		c[ImGuiCol_SeparatorActive] = t.accentHover;

		c[ImGuiCol_ResizeGrip] = t.surface;
		c[ImGuiCol_ResizeGripHovered] = A(t.accent, 0.55f);
		c[ImGuiCol_ResizeGripActive] = t.accent;

		c[ImGuiCol_FrameBg] = t.surface;
		c[ImGuiCol_FrameBgHovered] = A(t.accent, 0.24f);
		c[ImGuiCol_FrameBgActive] = A(t.accent, 0.39f);
		c[ImGuiCol_InputTextCursor] = t.textPrimary;

		c[ImGuiCol_Tab] = t.background;
		c[ImGuiCol_TabHovered] = A(t.accent, 0.31f);
		c[ImGuiCol_TabSelected] = t.surface;
		c[ImGuiCol_TabSelectedOverline] = t.accent;
		c[ImGuiCol_TabDimmed] = t.background;
		c[ImGuiCol_TabDimmedSelected] = t.surface;
		c[ImGuiCol_TabDimmedSelectedOverline] = A(t.accent, 0.31f);

		c[ImGuiCol_DockingPreview] = A(t.accent, 0.47f);
		c[ImGuiCol_DockingEmptyBg] = t.background;

		c[ImGuiCol_PlotLines] = t.textSecondary;
		c[ImGuiCol_PlotLinesHovered] = t.accent;
		c[ImGuiCol_PlotHistogram] = t.accent;
		c[ImGuiCol_PlotHistogramHovered] = t.accentHover;

		c[ImGuiCol_TableHeaderBg] = t.surfaceElevated;
		c[ImGuiCol_TableBorderStrong] = t.border;
		c[ImGuiCol_TableBorderLight] = A(t.border, 0.31f);
		c[ImGuiCol_TableRowBg] = t.background;
		c[ImGuiCol_TableRowBgAlt] = A(t.surface, 0.63f);

		c[ImGuiCol_TreeLines] = t.border;
		c[ImGuiCol_UnsavedMarker] = t.warning;
		c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.5f);
		c[ImGuiCol_DragDropTarget] = t.accent;
		c[ImGuiCol_DragDropTargetBg] = A(t.accent, 0.19f);
		c[ImGuiCol_NavCursor] = A(t.accent, 0.39f);
		c[ImGuiCol_NavWindowingHighlight] = A(t.textPrimary, 0.44f);
		c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.5f);

		// Multi-viewport OS windows must be opaque.
		c[ImGuiCol_WindowBg].w = 1.0f;
	}

	inline void ApplyTheme(const EditorTheme& theme)
	{
		ActiveTheme() = theme;
		RefreshTokens();
		ApplyImGuiColors(ImGui::GetStyle());
	}

	inline void TextSized(ImDrawList* drawList, const float size, const ImVec2 pos, const ImVec4& color, const char* text)
	{
		drawList->AddText(ImGui::GetFont(), size, pos, U32(color), text);
	}

	[[nodiscard]] inline ImVec2 MeasureSized(const float size, const char* text)
	{
		return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
	}

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

	inline void AccentHairline(ImDrawList* drawList, const ImVec2 min, const float width, const float alpha = 0.85f)
	{
		const float midX = min.x + width * 0.5f;
		const ImU32 solid = U32(WithAlpha(kAccent, alpha));
		const ImU32 clear = U32(WithAlpha(kAccent, 0.0f));
		drawList->AddRectFilledMultiColor(min, ImVec2(midX, min.y + 2.0f), clear, solid, solid, clear);
		drawList->AddRectFilledMultiColor(ImVec2(midX, min.y), ImVec2(min.x + width, min.y + 2.0f), solid, clear, clear, solid);
	}

	inline void PanelHeader(const char* eyebrow, const char* stat = nullptr)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float bandW = ImGui::GetContentRegionAvail().x;
		const float labelSize = ImGui::GetFontSize();
		const float statSize = ImGui::GetFontSize() * 0.82f;
		const float th = MeasureSized(labelSize, eyebrow).y;
		drawList->AddRectFilled(ImVec2(p.x, p.y + 1.0f), ImVec2(p.x + 3.0f, p.y + th), U32(kAccent));
		TextSized(drawList, labelSize, ImVec2(p.x + 11.0f, p.y), kMuted, eyebrow);
		if (stat != nullptr && stat[0] != '\0')
		{
			const ImVec2 sm = MeasureSized(statSize, stat);
			TextSized(drawList, statSize, ImVec2(p.x + bandW - sm.x, p.y + (th - sm.y) * 0.5f), kFaint, stat);
		}
		ImGui::Dummy(ImVec2(0.0f, th + 5.0f));
		AccentHairline(drawList, ImGui::GetCursorScreenPos(), bandW, 0.30f);
		ImGui::Dummy(ImVec2(0.0f, 4.0f));
	}

	inline void SectionTag(const char* label)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float size = ImGui::GetFontSize() * 0.92f;
		const float th = MeasureSized(size, label).y;
		drawList->AddRectFilled(ImVec2(pos.x, pos.y + 1.0f), ImVec2(pos.x + 3.0f, pos.y + th), U32(kAccent));
		TextSized(drawList, size, ImVec2(pos.x + 11.0f, pos.y), kMuted, label);
		ImGui::Dummy(ImVec2(11.0f + MeasureSized(size, label).x, th + 2.0f));
	}

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

	inline void CenterIconOnLastItem(const char* icon, const ImVec4& tint)
	{
		const ImVec2 bMin = ImGui::GetItemRectMin();
		const ImVec2 bMax = ImGui::GetItemRectMax();
		const ImVec2 centre((bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f);
		const unsigned char b0 = static_cast<unsigned char>(icon[0]);
		unsigned int codepoint = b0;
		if (b0 >= 0xF0u)
		{
			codepoint = ((b0 & 0x07u) << 18) | ((static_cast<unsigned char>(icon[1]) & 0x3Fu) << 12) | ((static_cast<unsigned char>(icon[2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(icon[3]) & 0x3Fu);
		}
		else if (b0 >= 0xE0u)
		{
			codepoint = ((b0 & 0x0Fu) << 12) | ((static_cast<unsigned char>(icon[1]) & 0x3Fu) << 6) | (static_cast<unsigned char>(icon[2]) & 0x3Fu);
		}
		else if (b0 >= 0xC0u)
		{
			codepoint = ((b0 & 0x1Fu) << 6) | (static_cast<unsigned char>(icon[1]) & 0x3Fu);
		}
		ImFontBaked* baked = ImGui::GetFontBaked();
		const ImFontGlyph* glyph = baked != nullptr ? baked->FindGlyph(static_cast<ImWchar>(codepoint)) : nullptr;
		if (glyph != nullptr)
		{
			const ImVec2 pos(std::floor(centre.x - (glyph->X0 + glyph->X1) * 0.5f), std::floor(centre.y - (glyph->Y0 + glyph->Y1) * 0.5f));
			ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(tint), icon);
		}
	}

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

	inline bool OutlineIconButton(const char* icon, const char* strId, const ImVec2 size, const ImVec4& tint = kAccentHi)
	{
		const bool pressed = OutlineButton(strId, size);
		CenterIconOnLastItem(icon, tint);
		return pressed;
	}

	inline bool GhostIconButton(const char* icon, const char* strId, const ImVec2 size, const ImVec4& tint = kMuted)
	{
		const bool pressed = GhostButton(strId, size, tint);
		CenterIconOnLastItem(icon, tint);
		return pressed;
	}

	inline bool ActiveToolButton(const char* label, const ImVec2 size)
	{
		return PrimaryButton(label, size);
	}
} // namespace aether::editor::chrome
