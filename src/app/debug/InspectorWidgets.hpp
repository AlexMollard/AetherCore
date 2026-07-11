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
	inline constexpr float kLabelWidth = 116.0f;

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

		ImGui::PushStyleColor(ImGuiCol_Header, ToImVec4(colors::SurfaceElevated));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, WithAlpha(colors::Orange, 0.22f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, WithAlpha(colors::Orange, 0.32f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 5.0f));
		const bool open = ImGui::CollapsingHeader(label, flags | ImGuiTreeNodeFlags_AllowOverlap);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);

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

	inline void PropLabel(const char* label)
	{
		const float startX = ImGui::GetCursorPosX();
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::TextSecondary));
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::SetCursorPosX(startX + kLabelWidth);
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

		const float startX = ImGui::GetCursorPosX();
		ImGui::AlignTextToFramePadding();
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::TextSecondary));
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::SetCursorPosX(startX + kLabelWidth);

		struct AxisChip
		{
			const char* tag;
			ImVec4 color;
			float* component;
		};
		AxisChip axes[3] = {
		        {"X", ImVec4(0.80f, 0.30f, 0.32f, 1.0f), &value.x},
		        {"Y", ImVec4(0.42f, 0.68f, 0.30f, 1.0f), &value.y},
		        {"Z", ImVec4(0.28f, 0.52f, 0.86f, 1.0f), &value.z},
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
			ImGui::PushStyleColor(ImGuiCol_Button, axes[i].color);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(axes[i].color.x * 1.2f, axes[i].color.y * 1.2f, axes[i].color.z * 1.2f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, axes[i].color);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
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
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(colors::Orange, 0.85f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, WithAlpha(colors::Orange, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.09f, 0.08f, 1.0f));
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
