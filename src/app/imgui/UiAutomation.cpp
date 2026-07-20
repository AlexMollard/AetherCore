#include "imgui/UiAutomation.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace aether::app
{
	UiAutomation& UiAutomation::Get()
	{
		static UiAutomation instance;
		return instance;
	}

	void UiAutomation::RecordItemAdd(unsigned int id, float x, float y, float w, float h, const char* window)
	{
		m_buildingIndex[id] = m_building.size();
		m_building.push_back(UiItem{id, std::string{}, window != nullptr ? window : "", x, y, w, h});
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
		const UiItem* match = nullptr;
		int matches = 0;
		for (const UiItem& item: m_snapshot)
		{
			if (item.label != label)
			{
				continue;
			}
			if (!window.empty() && item.window != window)
			{
				continue;
			}
			match = &item;
			++matches;
		}
		if (matches == 0)
		{
			err = "no widget labelled '" + label + "'" + (window.empty() ? "" : " in window '" + window + "'");
			return std::nullopt;
		}
		if (matches > 1)
		{
			err = "ambiguous: " + std::to_string(matches) + " widgets labelled '" + label + "' (pass a window to disambiguate)";
			return std::nullopt;
		}
		return *match;
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
	const char* window = (ctx != nullptr && ctx->CurrentWindow != nullptr) ? ctx->CurrentWindow->Name : "";
	aether::app::UiAutomation::Get().RecordItemAdd(static_cast<unsigned int>(id), bb.Min.x, bb.Min.y, bb.Max.x - bb.Min.x, bb.Max.y - bb.Min.y, window);
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
