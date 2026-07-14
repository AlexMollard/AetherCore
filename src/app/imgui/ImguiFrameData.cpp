#include "imgui/ImguiFrameData.hpp"
#include <imgui_internal.h>

namespace aether
{

	namespace
	{
		std::mutex s_imguiPoolMutex;
		std::vector<std::unique_ptr<ImguiFrameData>> s_imguiPool;
	} // namespace

	std::unique_ptr<ImguiFrameData> ImguiFrameData::AcquirePooled()
	{
		const std::lock_guard lock(s_imguiPoolMutex);
		if (!s_imguiPool.empty())
		{
			std::unique_ptr<ImguiFrameData> frame = std::move(s_imguiPool.back());
			s_imguiPool.pop_back();
			return frame;
		}
		return std::make_unique<ImguiFrameData>();
	}

	void ImguiFrameData::RecyclePooled(std::unique_ptr<ImguiFrameData> frame)
	{
		if (!frame)
		{
			return;
		}
		frame->Clear();
		const std::lock_guard lock(s_imguiPoolMutex);
		s_imguiPool.push_back(std::move(frame));
	}

	// Lifetime

	ImguiFrameData::~ImguiFrameData()
	{
		for (ImDrawList* list: m_mainPool)
		{
			IM_DELETE(list);
		}
		for (ImDrawList* list: m_secondaryPool)
		{
			IM_DELETE(list);
		}
	}

	void ImguiFrameData::Clear()
	{
		m_drawData.Clear();

		for (auto& vp: m_secondary)
		{
			vp.draw.Clear();
		}

		m_secondary.clear();
	}

	void ImguiFrameData::CloneInto(const ImDrawData* source, ImDrawData& dst, std::vector<ImDrawList*>& pool, std::size_t poolOffset, bool copyTextures)
	{
		static_cast<void>(copyTextures);

		dst.Clear();
		if (source == nullptr || !source->Valid)
		{
			return;
		}

		dst.Valid = true;
		dst.DisplayPos = source->DisplayPos;
		dst.DisplaySize = source->DisplaySize;
		dst.FramebufferScale = source->FramebufferScale;
		dst.OwnerViewport = source->OwnerViewport;

		// Texture updates are handled on the producer thread before capture.
		dst.Textures = nullptr;

		const int count = source->CmdListsCount;

		const std::size_t requiredSize = poolOffset + static_cast<std::size_t>(count);
		if (pool.size() < requiredSize)
		{
			const std::size_t oldSize = pool.size();
			pool.resize(requiredSize);
			for (std::size_t i = oldSize; i < requiredSize; ++i)
			{
				// Null shared data on purpose: these are render-thread copies that
				pool[i] = IM_NEW(ImDrawList)(nullptr);
			}
		}

		dst.CmdLists.resize(count);
		dst.CmdListsCount = count;
		dst.TotalVtxCount = 0;
		dst.TotalIdxCount = 0;

		for (int i = 0; i < count; ++i)
		{
			ImDrawList* dstList = pool[poolOffset + static_cast<std::size_t>(i)];

			if (source->CmdLists[i] == nullptr)
			{
				dstList->CmdBuffer.clear();
				dstList->VtxBuffer.clear();
				dstList->IdxBuffer.clear();
				dst.CmdLists[i] = dstList;
				continue;
			}

			const ImDrawList* srcList = source->CmdLists[i];

			dstList->CmdBuffer = srcList->CmdBuffer;
			dstList->VtxBuffer = srcList->VtxBuffer;
			dstList->IdxBuffer = srcList->IdxBuffer;

			dstList->Flags = srcList->Flags;
			// Deliberately NOT copying srcList->_Data: the clone must stay detached

			dst.CmdLists[i] = dstList;
			dst.TotalVtxCount += dstList->VtxBuffer.Size;
			dst.TotalIdxCount += dstList->IdxBuffer.Size;
		}
	}

	void ImguiFrameData::Capture(const ImDrawData* source)
	{
		Clear();
		CloneInto(source, m_drawData, m_mainPool, 0, /*copyTextures=*/true);
	}

	void ImguiFrameData::CaptureSecondary(const ImDrawData* source, ImGuiID id, ImVec2 pos, ImVec2 size, ImVec2 fbScale, void* platformHandle)
	{
		if (source == nullptr || !source->Valid || source->CmdListsCount <= 0)
		{
			return;
		}

		CapturedViewport vp;
		vp.id = id;
		vp.pos = pos;
		vp.size = size;
		vp.fbScale = fbScale;
		vp.platformHandle = platformHandle;
		vp.poolOffset = m_secondaryPool.size();
		vp.poolCount = static_cast<std::size_t>(source->CmdListsCount);

		CloneInto(source, vp.draw, m_secondaryPool, vp.poolOffset, /*copyTextures=*/false);
		m_secondary.push_back(std::move(vp));
	}

} // namespace aether
