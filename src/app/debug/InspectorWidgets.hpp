#pragma once

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Color.hpp"

namespace aether::editor::iw
{
	// Floor and ceiling for the label column. A fixed width was the reason "Linear damping",
	// "Max angular velocity" and "Sensor (trigger)" all truncated: 128px is too narrow for the
	// longer reflected field names, while a wide inspector had space going spare.
	inline constexpr float kLabelWidthMin = 140.0f;
	inline constexpr float kLabelWidthMax = 260.0f;

	// Measured from the window rather than the cursor so every call inside one row agrees,
	// including the drawers that position a second widget with SameLine.
	[[nodiscard]] inline float LabelWidth()
	{
		const float content = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
		// The floor stands down rather than starve the value field: in a panel narrow enough
		// that 140px of label would leave nothing to edit, half the width is the limit.
		const float floorWidth = std::min(kLabelWidthMin, content * 0.5f);
		return std::clamp(content * 0.42f, floorWidth, kLabelWidthMax);
	}

	// Identifies the component a section header belongs to, so the header can carry a
	// right-click menu. Held as an id and raw pointers rather than engine types: this header
	// is pure ImGui and every inspector drawer includes it.
	struct ComponentMenuTarget
	{
		void* services = nullptr;
		void* world = nullptr;
		std::uint32_t entityId = 0;
		// Null means "no menu" - sections that are not components (Tags, Hierarchy, the
		// particle editor's groupings) pass nothing and behave exactly as before.
		const char* component = nullptr;
	};

	// Defined in ComponentDrawers.cpp, which has the world and the undo stack. Declared here
	// so the shared header widgets can raise the menu on the header itself rather than on
	// whatever widget happened to be drawn last.
	void DrawComponentContextMenu(const ComponentMenuTarget& target);

	[[nodiscard]] inline ImVec4 ToImVec4(const glm::vec4& c)
	{
		return ImVec4(c.r, c.g, c.b, c.a);
	}

	[[nodiscard]] inline ImVec4 WithAlpha(const glm::vec4& c, float a)
	{
		return ImVec4(c.r, c.g, c.b, a);
	}

	inline void ItemTooltip(const char* text)
	{
		if (text != nullptr && text[0] != '\0')
		{
			ImGui::SetItemTooltip("%s", text);
		}
	}

	// inspect_component method on the main thread and consumed on the very next
	inline std::string& InspectorFocusRequest()
	{
		static std::string request;
		return request;
	}

	inline bool BeginSection(const char* label, ImGuiTreeNodeFlags flags)
	{
		std::string& focus = InspectorFocusRequest();
		bool wantFocus = false;
		if (!focus.empty())
		{
			const auto containsIgnoreCase = [](std::string_view haystack, std::string_view needle)
			{
				const auto lower = [](char c)
				{
					return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				};
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
				ImGui::SetNextItemOpen(true);
			}
		}

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
			ImGui::SetScrollHereY(0.08f);
			focus.clear();
		}
		return open;
	}

	inline bool SectionHeader(const char* label, ImGuiTreeNodeFlags flags = 0, const ComponentMenuTarget& menu = {})
	{
		const bool open = BeginSection(label, flags);
		DrawComponentContextMenu(menu);
		return open;
	}

	inline bool RemovableSection(const char* label, const char* removeId, bool& removed, ImGuiTreeNodeFlags flags = 0, const ComponentMenuTarget& menu = {})
	{
		const bool open = BeginSection(label, flags);
		// Raised before the remove button is drawn, or BeginPopupContextItem would bind to
		// the X instead of the header.
		DrawComponentContextMenu(menu);
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

	// Draws the label in its column so it can never bleed over the field to its right. A label
	// too long for the column is cut on a character boundary with an ellipsis and gets the full
	// text on hover - a hard mid-glyph cut ("Linear dampin") reads as a broken panel, where
	// "Linear damp..." reads as deliberate.
	inline void LabelColumn(const char* label)
	{
		ImGui::AlignTextToFramePadding();
		const float startX = ImGui::GetCursorPosX();
		const ImVec2 screenPos = ImGui::GetCursorScreenPos();
		const float column = LabelWidth();
		const float columnWidth = column - ImGui::GetStyle().ItemInnerSpacing.x;
		const bool truncated = ImGui::CalcTextSize(label).x > columnWidth;

		std::string shown;
		if (truncated)
		{
			// Only walks the string in the rare case it does not fit.
			shown = label;
			while (!shown.empty() && ImGui::CalcTextSize((shown + "...").c_str()).x > columnWidth)
			{
				shown.pop_back();
			}
			shown += "...";
		}

		const ImVec2 clipMin = screenPos;
		const ImVec2 clipMax(screenPos.x + columnWidth, screenPos.y + ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f);
		ImGui::PushStyleColor(ImGuiCol_Text, ToImVec4(colors::TextSecondary));
		ImGui::PushClipRect(clipMin, clipMax, true);
		ImGui::TextUnformatted(truncated ? shown.c_str() : label);
		ImGui::PopClipRect();
		ImGui::PopStyleColor();
		if (truncated && ImGui::IsMouseHoveringRect(clipMin, clipMax))
		{
			ImGui::SetTooltip("%s", label);
		}

		ImGui::SameLine(0.0f, 0.0f);
		ImGui::SetCursorPosX(startX + column);
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

	// callers don't juggle fixed char buffers. Same full-width, label-column layout.
	inline bool PropInputText(const char* label, std::string& str, const char* hint = nullptr)
	{
		ImGui::PushID(label);
		PropLabel(label);
		const bool changed = hint != nullptr ? ImGui::InputTextWithHint("##txt", hint, &str) : ImGui::InputText("##txt", &str);
		ImGui::PopID();
		return changed;
	}

	inline void PropText(const char* label, const char* fmt, ...)
	{
		// Shares LabelColumn so a long label ellipsises here too instead of shoving the value
		// out of alignment with every other row.
		LabelColumn(label);
		va_list args = nullptr;
		va_start(args, fmt);
		ImGui::TextV(fmt, args);
		va_end(args);
	}

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

		const AxisChip axes[3] = {
		        {"X", ToImVec4(colors::AxisX), &value.x},
		        {"Y", ToImVec4(colors::AxisY), &value.y},
		        {"Z", ToImVec4(colors::AxisZ), &value.z},
		};

		const float chipWidth = ImGui::GetFrameHeight();
		const float spacing = 4.0f;
		const float avail = ImGui::GetContentRegionAvail().x;
		const float spacingTotal = 2.0f * spacing;

		// A minimum field width used to be forced here, which meant three axes plus their
		// chips could add up to more than the row had - and the Z axis simply rendered off
		// the right-hand edge, so a narrow inspector showed one number out of three. Nothing
		// below may exceed `avail`.
		const float widthWithChips = (avail - 3.0f * chipWidth - spacingTotal) / 3.0f;
		// The chips are the first thing to go: an unreadable field is worse than a missing
		// reset button, and the axis stays identifiable by the tint below.
		const bool showChips = widthWithChips >= 34.0f;
		// No floor at all: any minimum large enough to matter is a minimum large enough to
		// push the last axis off the edge, which is the bug this replaced.
		const float fieldWidth = std::max(1.0f, showChips ? widthWithChips : (avail - spacingTotal) / 3.0f);

		for (int i = 0; i < 3; ++i)
		{
			if (i > 0)
			{
				ImGui::SameLine(0.0f, spacing);
			}
			if (showChips)
			{
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
			}
			else
			{
				// Without its chip a field would be an anonymous box, so the frame carries
				// the axis colour instead.
				ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(axes[i].color.x, axes[i].color.y, axes[i].color.z, 0.18f));
			}
			ImGui::SetNextItemWidth(fieldWidth);
			char dragId[8];
			std::snprintf(dragId, sizeof(dragId), "##d%d", i);
			changed |= ImGui::DragFloat(dragId, axes[i].component, speed);
			if (!showChips)
			{
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s", axes[i].tag);
				}
			}
		}

		ImGui::PopID();
		return changed;
	}

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
} // namespace aether::editor::iw
