#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <mutex>
#include <imgui.h>

#include "rendering/IUiOverlay.hpp"

namespace aether
{
	// base, so it never needs to know Dear ImGui exists.
	class ImguiFrameData : public IUiOverlayFrameData
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
		~ImguiFrameData() override;

		ImguiFrameData(const ImguiFrameData&) = delete;
		ImguiFrameData& operator=(const ImguiFrameData&) = delete;

		void Clear();
		void Capture(const ImDrawData* source);
		void CaptureSecondary(const ImDrawData* source, ImGuiID id, ImVec2 pos, ImVec2 size, ImVec2 fbScale, void* platformHandle);

		// identity - and its warm ImDrawList* pools - never moves; handing one
		[[nodiscard]] static std::unique_ptr<ImguiFrameData> AcquirePooled();
		static void RecyclePooled(std::unique_ptr<ImguiFrameData> frame);

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

		std::vector<ImDrawList*> m_mainPool;
		std::vector<ImDrawList*> m_secondaryPool;

		std::vector<CapturedViewport> m_secondary;
	};

} // namespace aether
