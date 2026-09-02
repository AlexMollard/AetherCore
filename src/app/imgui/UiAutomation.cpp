#include "imgui/UiAutomation.hpp"

#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace aether::app
{
	UiAutomation& UiAutomation::Get()
	{
		static UiAutomation instance;
		return instance;
	}

	void UiAutomation::RecordItemAdd(unsigned int id, float x, float y, float w, float h, const char* window, bool clipped)
	{
		m_buildingIndex[id] = m_building.size();
		m_building.push_back(UiItem{id, std::string{}, window != nullptr ? window : "", x, y, w, h, clipped});
	}

	void UiAutomation::RecordItemInfo(unsigned int id, const char* label)
	{
		const auto it = m_buildingIndex.find(id);
		if (it != m_buildingIndex.end() && label != nullptr)
		{
			m_building[it->second].label = label;
		}
	}

	const char* UiAutomation::DebugLabel(unsigned int id) const
	{
		for (const UiItem& item: m_snapshot)
		{
			if (item.id == id && !item.label.empty())
			{
				return item.label.c_str();
			}
		}
		return "";
	}

	void UiAutomation::BeginFrameSwap()
	{
		m_snapshot = std::move(m_building);
		m_building.clear();
		m_buildingIndex.clear();
	}

	std::optional<UiItem> UiAutomation::FindItem(const std::string& window, const std::string& label, std::string& err) const
	{
		// Exact first, then substring. ui_query is the documented way to find a target and
		// filters by substring, so what it hands back has to work here - and what it hands
		// back is ImGui's real label, carrying "###id" suffixes, icon glyphs and whatever
		// the panel appended. Matching only exactly meant the discovery tool and the action
		// tool disagreed about what a filter means, and the values you had just been shown
		// were rejected.
		//
		// Exact is still tried first so a label that is a prefix of a longer one ("Save"
		// beside "Save As...") resolves to itself rather than reporting an ambiguity.
		for (const bool exact: {true, false})
		{
			std::vector<const UiItem*> hits;
			for (const UiItem& item: m_snapshot)
			{
				const bool labelMatches = exact ? item.label == label : item.label.find(label) != std::string::npos;
				if (!labelMatches)
				{
					continue;
				}
				const bool windowMatches = window.empty() || (exact ? item.window == window : item.window.find(window) != std::string::npos);
				if (!windowMatches)
				{
					continue;
				}
				hits.push_back(&item);
			}

			if (hits.size() == 1)
			{
				return *hits.front();
			}
			if (hits.size() > 1)
			{
				// Name the candidates: "pass a window to disambiguate" is no help when the
				// windows are what you cannot see from here.
				err = "ambiguous: " + std::to_string(hits.size()) + " widgets match label '" + label + "'";
				constexpr std::size_t kMaxListed = 6;
				for (std::size_t i = 0; i < hits.size() && i < kMaxListed; ++i)
				{
					err += "\n  '" + hits[i]->label + "' in '" + hits[i]->window + "'";
				}
				if (hits.size() > kMaxListed)
				{
					err += "\n  ... and " + std::to_string(hits.size() - kMaxListed) + " more";
				}
				return std::nullopt;
			}
		}

		err = "no widget labelled '" + label + "'" + (window.empty() ? "" : " in window '" + window + "'");
		return std::nullopt;
	}

	void UiAutomation::ApplyInput(ImGuiIO& io)
	{
		for (const SynEvent& e: m_input.Step())
		{
			switch (e.kind)
			{
				case SynKind::MousePos:
					io.AddMousePosEvent(e.x, e.y);
					break;
				case SynKind::MouseButton:
					io.AddMouseButtonEvent(e.button, e.down);
					break;
				case SynKind::Key:
					io.AddKeyEvent(static_cast<ImGuiKey>(e.key), e.down);
					break;
				case SynKind::Char:
					io.AddInputCharacter(e.ch);
					break;
			}
		}
	}
} // namespace aether::app

// ── ImGui test-engine hook symbols ──────────────────────────────────────────
// Required by imgui.cpp when IMGUI_ENABLE_TEST_ENGINE is defined. Forward every
// item into the singleton registry. Signatures must match imgui_internal.h.

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*)
{
	const ImGuiWindow* current = (ctx != nullptr) ? ctx->CurrentWindow : nullptr;
	const char* window = (current != nullptr) ? current->Name : "";
	// ImGui clips drawing to this rect, so anything of the item outside it is invisible.
	// Partially outside only: an item entirely outside has simply been scrolled out of
	// view, which is ordinary and would drown the genuinely half-drawn ones in noise.
	const bool clipped = current != nullptr && current->ClipRect.Overlaps(bb) && !current->ClipRect.Contains(bb);
	aether::app::UiAutomation::Get().RecordItemAdd(static_cast<unsigned int>(id), bb.Min.x, bb.Min.y, bb.Max.x - bb.Min.x, bb.Max.y - bb.Min.y, window, clipped);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
	aether::app::UiAutomation::Get().RecordItemInfo(static_cast<unsigned int>(id), label != nullptr ? label : "");
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id)
{
	return aether::app::UiAutomation::Get().DebugLabel(static_cast<unsigned int>(id));
}
