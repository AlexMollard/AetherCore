#pragma once

#include <cstdint>
#include <vector>

#include <imgui.h>

namespace aether
{
	// Thread-transferable snapshot of an ImGui frame's draw data (main viewport +
	// every secondary/OS-window viewport when multi-viewport is enabled).
	class ImguiFrameData
	{
	public:
		// One torn-out OS-window viewport, deep-copied for the render thread.
		struct CapturedViewport
		{
			ImGuiID id = 0;
			ImDrawData draw{};
			std::vector<ImDrawList*> owned;
			ImVec2 pos{0.0f, 0.0f};
			ImVec2 size{0.0f, 0.0f};
			ImVec2 fbScale{1.0f, 1.0f};
			void* platformHandle = nullptr; // GLFWwindow*, created on the producer thread
		};

		ImguiFrameData() = default;
		~ImguiFrameData();

		ImguiFrameData(const ImguiFrameData&) = delete;
		ImguiFrameData& operator=(const ImguiFrameData&) = delete;
		ImguiFrameData(ImguiFrameData&& other) noexcept;
		ImguiFrameData& operator=(ImguiFrameData&& other) noexcept;

		void Clear();
		void Capture(const ImDrawData* source); // main viewport
		void CaptureSecondary(const ImDrawData* source, ImGuiID id, ImVec2 pos, ImVec2 size, ImVec2 fbScale, void* platformHandle);

		[[nodiscard]] bool HasDrawData() const noexcept
		{
			return m_drawData.Valid && m_drawData.CmdListsCount > 0 && m_drawData.TotalVtxCount > 0;
		}

		[[nodiscard]] ImDrawData* GetDrawData() noexcept { return &m_drawData; }
		[[nodiscard]] const ImDrawData* GetDrawData() const noexcept { return &m_drawData; }

		[[nodiscard]] const std::vector<CapturedViewport>& SecondaryViewports() const noexcept
		{
			return m_secondary;
		}

	private:
		void RebuildCommandListView();
		static void CloneInto(const ImDrawData* source, ImDrawData& dst, std::vector<ImDrawList*>& owned, bool copyTextures);
		static void RebuildView(ImDrawData& dst, std::vector<ImDrawList*>& owned);

		ImDrawData m_drawData;
		std::vector<ImDrawList*> m_ownedLists;
		std::vector<CapturedViewport> m_secondary;
	};
} // namespace aether
