#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui/UiInputScript.hpp"

struct ImGuiIO;

namespace aether::app
{
	// One widget recorded from the current ImGui frame. Rect is screen space.
	struct UiItem
	{
		unsigned int id = 0;
		std::string label;
		std::string window;
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
	};

	// Global singleton bridging ImGui's test-engine item hooks and synthetic input
	// to the MCP control methods. Lives on the main (loop) thread; all access is
	// from there (ImguiSubsystem per-frame + ControlServer::DrainCommands).
	class UiAutomation
	{
	public:
		static UiAutomation& Get();

		// Per-frame, from ImguiSubsystem::BeginFrame:
		void BeginFrameSwap();      // publish building -> snapshot, clear building
		void ApplyInput(ImGuiIO& io); // drain the input script into io events

		// Reads (main thread):
		[[nodiscard]] const std::vector<UiItem>& Snapshot() const { return m_snapshot; }
		[[nodiscard]] std::optional<UiItem> FindItem(const std::string& window, const std::string& label, std::string& err) const;
		[[nodiscard]] UiInputScript& Input() { return m_input; }

		// Hook sinks (called from the ImGuiTestEngineHook_* symbols):
		void RecordItemAdd(unsigned int id, float x, float y, float w, float h, const char* window);
		void RecordItemInfo(unsigned int id, const char* label);
		[[nodiscard]] const char* DebugLabel(unsigned int id) const;

	private:
		UiAutomation() = default;

		std::vector<UiItem> m_building;
		std::unordered_map<unsigned int, std::size_t> m_buildingIndex;
		std::vector<UiItem> m_snapshot;
		std::string m_debugLabel; // scratch for DebugLabel return
		UiInputScript m_input;
	};
} // namespace aether::app
