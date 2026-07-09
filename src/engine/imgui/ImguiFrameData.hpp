#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
#include <imgui.h>

namespace aether
{
	class ImguiFrameData
	{
	public:
		struct CapturedViewport
		{
			ImGuiID id = 0;
			ImDrawData draw{};
			ImVec2 pos{0.0f, 0.0f};
			ImVec2 size{0.0f, 0.0f};
			ImVec2 fbScale{1.0f, 1.0f};
			void* platformHandle = nullptr;

			std::size_t poolOffset = 0;
			std::size_t poolCount = 0;
		};

		ImguiFrameData() = default;
		~ImguiFrameData();

		ImguiFrameData(const ImguiFrameData&) = delete;
		ImguiFrameData& operator=(const ImguiFrameData&) = delete;
		ImguiFrameData(ImguiFrameData&& other) noexcept;
		ImguiFrameData& operator=(ImguiFrameData&& other) noexcept;

		void Clear();
		void Capture(const ImDrawData* source);
		void CaptureSecondary(const ImDrawData* source, ImGuiID id, ImVec2 pos, ImVec2 size, ImVec2 fbScale, void* platformHandle);

		// --- Object Pooling API ---
		[[nodiscard]] static ImguiFrameData AcquirePooled();
		static void RecyclePooled(ImguiFrameData&& frame);

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

		[[nodiscard]] const std::vector<CapturedViewport>& SecondaryViewports() const noexcept
		{
			return m_secondary;
		}

	private:
		void CloneInto(const ImDrawData* source, ImDrawData& dst, std::vector<ImDrawList*>& pool, std::size_t poolOffset, bool copyTextures);

		ImDrawData m_drawData;

		// Persistent pools to avoid heap allocations every frame.
		std::vector<ImDrawList*> m_mainPool;
		std::vector<ImDrawList*> m_secondaryPool;

		std::vector<CapturedViewport> m_secondary;
	};

} // namespace aether
