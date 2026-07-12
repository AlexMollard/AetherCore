#pragma once

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <imgui.h>

#include "Color.hpp"

// ── Inspector widget toolkit ──────────────────────────────────────────────────
//
// A small, consistent set of property widgets and section headers built on the
// engine's `colors::` design tokens (Color.hpp). The goal is a uniform look for
// every component drawer: one label column, full-width fields, themed section
// headers, and shared accent/danger buttons - so the inspector stops feeling
// hand-rolled per component. All helpers are inline and free of engine deps
// beyond ImGui + the colour palette, so both InspectorPanel and ComponentDrawers
// can share them.

namespace aether::app::iw
{
	// Width of the label column in property rows. Fields fill the remainder.
	inline constexpr float kLabelWidth = 128.0f;

	[[nodiscard]] inline ImVec4 ToImVec4(const glm::vec4& c)
	{
		return ImVec4(c.r, c.g, c.b, c.a);
	}

	[[nodiscard]] inline ImVec4 WithAlpha(const glm::vec4& c, float a)
	{
		return ImVec4(c.r, c.g, c.b, a);
	}

	// Tooltip on the previous item when `text` is non-null/non-empty.
	inline void ItemTooltip(const char* text)
	{
		if (text != nullptr && text[0] != '\0')
		{
			ImGui::SetItemTooltip("%s", text);
		}
	}

	// ── Inspector focus (control endpoint / MCP) ──────────────────────────────
	// One-shot request: the next component section whose label contains this text
	// (case-insensitive; the icon + spacing prefix is ignored) force-opens and
	// scrolls itself to the top of the Inspector. Set by the endpoint's
	// inspect_component method on the main thread and consumed on the very next
	// inspector draw (same thread), so no locking is needed. Empty => no request.
	// A free-function-local static is the pragmatic home: every drawer funnels
	// through the shared BeginSection below, which has no InspectorPanel handle.
	inline std::string& InspectorFocusRequest()
	{
		static std::string request;
		return request;
	}

	// ── Section headers ───────────────────────────────────────────────────────
	// Themed collapsing header shared by every component section. Removable
	// variant adds a right-aligned remove control; both render identically.

	inline bool BeginSection(const char* label, ImGuiTreeNodeFlags flags)
	{
		std::string& focus = InspectorFocusRequest();
		bool wantFocus = false;
		if (!focus.empty())
		{
			const auto containsIgnoreCase = [](std::string_view haystack, std::string_view needle)
			{
				const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
				if (needle.empty() || needle.size() > haystack.size())
				{
					return false;
				}
				for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
				{
					std::size_t j = 0;
					for (; j < needle.size(); ++j)
					{
						if (lower(haystack[i + j]) != lower(needle[j]))
						{
							break;
						}
					}
					if (j == needle.size())
					{
						return true;
					}
				}
				return false;
			};
			wantFocus = containsIgnoreCase(label, focus);
			if (wantFocus)
			{
				ImGui::SetNextItemOpen(true); // expand so the drawer body is visible
			}
		}

		// Launcher language: flat headers (no boxes) - an open section carries a
		// 3px amber tick on its leading edge, and a hairline under every header
		// keeps sections separated without nesting surfaces.
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, WithAlpha(colors::Orange, 0.14f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, WithAlpha(colors::Orange, 0.24f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 6.0f));
		const bool open = ImGui::CollapsingHeader(label, flags | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		{
			ImDrawList* headerDrawList = ImGui::GetWindowDrawList();
			const ImVec2 hMin = ImGui::GetItemRectMin();
			const ImVec2 hMax = ImGui::GetItemRectMax();
			if (open)
			{
				headerDrawList->AddRectFilled(ImVec2(hMin.x, hMin.y + 4.0f), ImVec2(hMin.x + 3.0f, hMax.y - 4.0f), ImGui::ColorConvertFloat4ToU32(ToImVec4(colors::Primary)));
			}
			headerDrawList->AddLine(ImVec2(hMin.x, hMax.y), ImVec2(hMax.x, hMax.y), ImGui::ColorConvertFloat4ToU32(WithAlpha(colors::Border, 0.9f)), 1.0f);
		}

		if (wantFocus)
		{
			ImGui::SetScrollHereY(0.08f); // bring this header near the top of the Inspector
			focus.clear();                // one-shot
		}
		return open;
	}

	// Non-removable section header (Transform, Material, ...).
	inline bool SectionHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
	{
		return BeginSection(label, flags);
	}

	// Removable section header. `removeId` carries its own icon + "##id" suffix.
	// AllowOverlap is load-bearing: the header is submitted first as a full-row
	// item, so the remove button underneath needs overlap to be clickable.
	inline bool RemovableSection(const char* label, const char* removeId, bool& removed, ImGuiTreeNodeFlags flags = 0)
	{
		const bool open = BeginSection(label, flags);
		ImGui::SameLine();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 22.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::TextSecondary));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(colors::Red, 0.25f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(colors::Red, 0.4f));
		removed = ImGui::SmallButton(removeId);
		ImGui::PopStyleColor(4);
		return open;
	}

	// ── Property rows ─────────────────────────────────────────────────────────
	// Every row draws a muted label in the fixed label column, then a field that
	// fills the rest of the width. PushID(label) keeps the "##" field id unique.

	// Draws a muted label clipped to the label column, so a label longer than the
	// column can never bleed over the field to its right. Truncated labels get a
	// hover tooltip with the full text. The field always starts at startX +
	// kLabelWidth and fills the remaining width.
	inline void LabelColumn(const char* label)
	{
		ImGui::AlignTextToFramePadding();
		const float startX = ImGui::GetCursorPosX();
		const ImVec2 screenPos = ImGui::GetCursorScreenPos();
		const float columnWidth = kLabelWidth - ImGui::GetStyle().ItemInnerSpacing.x;
		const bool truncated = ImGui::CalcTextSize(label).x > columnWidth;

		const ImVec2 clipMin = screenPos;
		const ImVec2 clipMax(screenPos.x + columnWidth, screenPos.y + ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::TextSecondary));
		ImGui::PushClipRect(clipMin, clipMax, true);
		ImGui::TextUnformatted(label);
		ImGui::PopClipRect();
		ImGui::PopStyleColor();
		if (truncated && ImGui::IsMouseHoveringRect(clipMin, clipMax))
		{
			ImGui::SetTooltip("%s", label);
		}

		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorPosX(startX + kLabelWidth);
	}

	inline void PropLabel(const char* label)
	{
		LabelColumn(label);
		ImGui::SetNextItemWidth(-FLT_MIN);
	}

	inline bool PropFloat(const char* label, float* v, float speed = 0.05f, float min = 0.0f, float max = 0.0f, const char* fmt = "%.3f", const char* tooltip = nullptr)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::DragFloat("##f", v, speed, min, max, fmt);
		ItemTooltip(tooltip);
		ImGui::PopID();
		return changed;
	}

	inline bool PropSlider(const char* label, float* v, float min, float max, const char* fmt = "%.3f", const char* tooltip = nullptr)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::SliderFloat("##s", v, min, max, fmt);
		ItemTooltip(tooltip);
		ImGui::PopID();
		return changed;
	}

	inline bool PropInt(const char* label, int* v, float speed = 1.0f, int min = 0, int max = 0, const char* tooltip = nullptr)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::DragInt("##i", v, speed, min, max);
		ItemTooltip(tooltip);
		ImGui::PopID();
		return changed;
	}

	inline bool PropDrag2(const char* label, float* v, float speed = 0.05f, float min = 0.0f, float max = 0.0f, const char* fmt = "%.2f")
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::DragFloat2("##d2", v, speed, min, max, fmt);
		ImGui::PopID();
		return changed;
	}

	// Combo over a "\0"-separated item string (e.g. "Left\0Center\0Right\0").
	inline bool PropComboStr(const char* label, int* index, const char* itemsZeroSep)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::Combo("##comboz", index, itemsZeroSep);
		ImGui::PopID();
		return changed;
	}

	inline bool PropCheckbox(const char* label, bool* v, const char* tooltip = nullptr)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::Checkbox("##c", v);
		ItemTooltip(tooltip);
		ImGui::PopID();
		return changed;
	}

	inline bool PropColor3(const char* label, float* rgb)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::ColorEdit3("##col3", rgb, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
		ImGui::PopID();
		return changed;
	}

	inline bool PropColor4(const char* label, float* rgba)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = ImGui::ColorEdit4("##col4", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_Float);
		ImGui::PopID();
		return changed;
	}

	inline bool PropCombo(const char* label, int* index, const char* const items[], int count, const char* tooltip = nullptr)
	{
		ImGui::PushID(label);
		PropLabel(label);
		bool changed = false;
		const char* preview = (*index >= 0 && *index < count) ? items[*index] : "";
		if (ImGui::BeginCombo("##combo", preview))
		{
			for (int i = 0; i < count; ++i)
			{
				const bool selected = *index == i;
				if (ImGui::Selectable(items[i], selected))
				{
					*index = i;
					changed = true;
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ItemTooltip(tooltip);
		ImGui::PopID();
		return changed;
	}

	inline bool PropInputText(const char* label, char* buf, std::size_t size, const char* hint = nullptr)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = hint != nullptr ? ImGui::InputTextWithHint("##txt", hint, buf, size) : ImGui::InputText("##txt", buf, size);
		ImGui::PopID();
		return changed;
	}

	// Read-only muted value row (e.g. "GPU slot  3").
	inline void PropText(const char* label, const char* fmt, ...)
	{
		const float startX = ImGui::GetCursorPosX();
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::TextSecondary));
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::SetCursorPosX(startX + kLabelWidth);
		va_list args;
		va_start(args, fmt);
		ImGui::TextV(fmt, args);
		va_end(args);
	}

	// ── Vector row ────────────────────────────────────────────────────────────
	// Unity-style RGB axis chips (click a chip to reset that axis) with per-axis
	// drags sharing the field column. Returns true when any component changed.
	inline bool Vec3Row(const char* label, glm::vec3& value, float resetValue = 0.0f, float speed = 0.05f)
	{
		bool changed = false;
		ImGui::PushID(label);

		LabelColumn(label);

		struct AxisChip
		{
			const char* tag;
			ImVec4 color;
			float* component;
		};
		AxisChip axes[3] = {
		        {"X", ToImVec4(colors::AxisX), &value.x},
		        {"Y", ToImVec4(colors::AxisY), &value.y},
		        {"Z", ToImVec4(colors::AxisZ), &value.z},
		};

		const float chipWidth = ImGui::GetFrameHeight();
		const float spacing = 4.0f;
		const float fieldWidth = std::max(38.0f, (ImGui::GetContentRegionAvail().x - 3.0f * chipWidth - 2.0f * spacing) / 3.0f);

		for (int i = 0; i < 3; ++i)
		{
			if (i > 0)
			{
				ImGui::SameLine(0.0f, spacing);
			}
			// Tinted-ghost chips (the editor's quiet-control language): a low-alpha
			// tint carries the axis identity, the letter takes the full axis colour,
			// and the fill only strengthens under the cursor. Solid full-bleed chips
			// read as generic-editor primaries against the Night Amber palette.
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(axes[i].color.x, axes[i].color.y, axes[i].color.z, 0.22f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(axes[i].color.x, axes[i].color.y, axes[i].color.z, 0.45f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(axes[i].color.x, axes[i].color.y, axes[i].color.z, 0.65f));
			ImGui::PushStyleColor(ImGuiCol_Text, axes[i].color);
			if (ImGui::Button(axes[i].tag, ImVec2(chipWidth, 0.0f)))
			{
				*axes[i].component = resetValue;
				changed = true;
			}
			ImGui::PopStyleColor(4);
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::SetNextItemWidth(fieldWidth);
			char dragId[8];
			std::snprintf(dragId, sizeof(dragId), "##d%d", i);
			changed |= ImGui::DragFloat(dragId, axes[i].component, speed);
		}

		ImGui::PopID();
		return changed;
	}

	// ── Buttons ───────────────────────────────────────────────────────────────

	inline bool AccentButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f))
	{
		ImGui::PushStyleColor(ImGuiCol_Button, ToImVec4(colors::Orange));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ToImVec4(colors::PrimaryHover));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToImVec4(colors::PrimaryActive));
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::OnPrimary));
		const bool clicked = ImGui::Button(label, size);
		ImGui::PopStyleColor(4);
		return clicked;
	}

	inline bool DangerButton(const char* label, const ImVec2& size = ImVec2(0.0f, 0.0f))
	{
		ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(colors::Red, 0.18f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(colors::Red, 0.75f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToImVec4(colors::Red));
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::TextPrimary));
		const bool clicked = ImGui::Button(label, size);
		ImGui::PopStyleColor(4);
		return clicked;
	}
} // namespace aether::app::iw
