#include "imgui/ImguiFrameData.hpp"

namespace aether
{
	ImguiFrameData::~ImguiFrameData()
	{
		Clear();
	}

	ImguiFrameData::ImguiFrameData(ImguiFrameData&& other) noexcept
	      : m_drawData(other.m_drawData), m_ownedLists(std::move(other.m_ownedLists)), m_secondary(std::move(other.m_secondary))
	{
		RebuildView(m_drawData, m_ownedLists);
		for (auto& vp: m_secondary)
		{
			RebuildView(vp.draw, vp.owned);
		}
		other.m_drawData.Clear();
		other.m_ownedLists.clear();
		other.m_secondary.clear();
	}

	ImguiFrameData& ImguiFrameData::operator=(ImguiFrameData&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}
		Clear();
		m_drawData = other.m_drawData;
		m_ownedLists = std::move(other.m_ownedLists);
		m_secondary = std::move(other.m_secondary);
		RebuildView(m_drawData, m_ownedLists);
		for (auto& vp: m_secondary)
		{
			RebuildView(vp.draw, vp.owned);
		}
		other.m_drawData.Clear();
		other.m_ownedLists.clear();
		other.m_secondary.clear();
		return *this;
	}

	void ImguiFrameData::Clear()
	{
		for (ImDrawList* list: m_ownedLists)
		{
			IM_DELETE(list);
		}
		m_ownedLists.clear();
		m_drawData.Clear();

		for (auto& vp: m_secondary)
		{
			for (ImDrawList* list: vp.owned)
			{
				IM_DELETE(list);
			}
		}
		m_secondary.clear();
	}

	void ImguiFrameData::CloneInto(const ImDrawData* source, ImDrawData& dst, std::vector<ImDrawList*>& owned)
	{
		dst.Clear();
		owned.clear();
		if (source == nullptr || !source->Valid)
		{
			return;
		}

		dst.Valid = true;
		dst.DisplayPos = source->DisplayPos;
		dst.DisplaySize = source->DisplaySize;
		dst.FramebufferScale = source->FramebufferScale;
		dst.OwnerViewport = source->OwnerViewport;
		dst.Textures = source->Textures;

		owned.reserve(static_cast<std::size_t>(source->CmdListsCount));
		for (const ImDrawList* sourceList: source->CmdLists)
		{
			if (sourceList == nullptr)
			{
				continue;
			}
			ImDrawList* clone = sourceList->CloneOutput();
			dst.CmdLists.push_back(clone);
			owned.push_back(clone);
			dst.CmdListsCount = dst.CmdLists.Size;
			dst.TotalVtxCount += clone->VtxBuffer.Size;
			dst.TotalIdxCount += clone->IdxBuffer.Size;
		}
	}

	void ImguiFrameData::RebuildView(ImDrawData& dst, std::vector<ImDrawList*>& owned)
	{
		dst.CmdLists.resize(0);
		dst.CmdLists.reserve(static_cast<int>(owned.size()));
		dst.CmdListsCount = 0;
		for (ImDrawList* list: owned)
		{
			dst.CmdLists.push_back(list);
			dst.CmdListsCount = dst.CmdLists.Size;
		}
	}

	void ImguiFrameData::Capture(const ImDrawData* source)
	{
		Clear();
		CloneInto(source, m_drawData, m_ownedLists);
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
		CloneInto(source, vp.draw, vp.owned);
		m_secondary.push_back(std::move(vp));
	}

	void ImguiFrameData::RebuildCommandListView()
	{
		RebuildView(m_drawData, m_ownedLists);
	}
} // namespace aether
