#include "imgui/ImguiFrameData.hpp"

namespace aether
{
	ImguiFrameData::~ImguiFrameData()
	{
		Clear();
	}

	ImguiFrameData::ImguiFrameData(ImguiFrameData&& other) noexcept
	      : m_drawData(other.m_drawData), m_ownedLists(std::move(other.m_ownedLists))
	{
		RebuildCommandListView();
		other.m_drawData.Clear();
		other.m_ownedLists.clear();
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
		RebuildCommandListView();

		other.m_drawData.Clear();
		other.m_ownedLists.clear();
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
	}

	void ImguiFrameData::Capture(const ImDrawData* source)
	{
		Clear();
		if (source == nullptr || !source->Valid)
		{
			return;
		}

		m_drawData.Valid = true;
		m_drawData.DisplayPos = source->DisplayPos;
		m_drawData.DisplaySize = source->DisplaySize;
		m_drawData.FramebufferScale = source->FramebufferScale;
		m_drawData.OwnerViewport = source->OwnerViewport;
		m_drawData.Textures = source->Textures;

		m_ownedLists.reserve(static_cast<std::size_t>(source->CmdListsCount));
		for (const ImDrawList* sourceList: source->CmdLists)
		{
			if (sourceList == nullptr)
			{
				continue;
			}

			ImDrawList* clone = sourceList->CloneOutput();
			m_drawData.CmdLists.push_back(clone);
			m_ownedLists.push_back(clone);
			m_drawData.CmdListsCount = m_drawData.CmdLists.Size;
			m_drawData.TotalVtxCount += clone->VtxBuffer.Size;
			m_drawData.TotalIdxCount += clone->IdxBuffer.Size;
		}
	}

	void ImguiFrameData::RebuildCommandListView()
	{
		m_drawData.CmdLists.resize(0);
		m_drawData.CmdLists.reserve(static_cast<int>(m_ownedLists.size()));
		m_drawData.CmdListsCount = 0;
		for (ImDrawList* list: m_ownedLists)
		{
			m_drawData.CmdLists.push_back(list);
			m_drawData.CmdListsCount = m_drawData.CmdLists.Size;
		}
	}
} // namespace aether
