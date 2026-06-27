#pragma once

#include <vector>

#include <imgui.h>

namespace aether
{
	// Thread-transferable snapshot of an ImGui frame's draw data.
	class ImguiFrameData
	{
	public:
		ImguiFrameData() = default;
		~ImguiFrameData();

		ImguiFrameData(const ImguiFrameData&) = delete;
		ImguiFrameData& operator=(const ImguiFrameData&) = delete;
		ImguiFrameData(ImguiFrameData&& other) noexcept;
		ImguiFrameData& operator=(ImguiFrameData&& other) noexcept;

		void Clear();
		void Capture(const ImDrawData* source);

		[[nodiscard]] bool HasDrawData() const noexcept
		{
			return m_drawData.Valid && m_drawData.CmdListsCount > 0 && m_drawData.TotalVtxCount > 0;
		}

		[[nodiscard]] ImDrawData* GetDrawData() noexcept
		{
			return &m_drawData;
		}

		[[nodiscard]] const ImDrawData* GetDrawData() const noexcept
		{
			return &m_drawData;
		}

	private:
		void RebuildCommandListView();

		ImDrawData m_drawData;
		std::vector<ImDrawList*> m_ownedLists;
	};
} // namespace aether
