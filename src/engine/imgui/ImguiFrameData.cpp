#include "imgui/ImguiFrameData.hpp"
#include <imgui_internal.h>

namespace aether
{

	// ---------------------------------------------------------------------------
	// Global Object Pool for ImguiFrameData
	// ---------------------------------------------------------------------------
	namespace
	{
		std::mutex s_imguiPoolMutex;
		std::vector<ImguiFrameData> s_imguiPool;
	} // namespace

	ImguiFrameData ImguiFrameData::AcquirePooled()
	{
		std::lock_guard lock(s_imguiPoolMutex);
		if (!s_imguiPool.empty())
		{
			ImguiFrameData frame = std::move(s_imguiPool.back());
			s_imguiPool.pop_back();
			return frame;
		}
		return ImguiFrameData();
	}

	void ImguiFrameData::RecyclePooled(ImguiFrameData&& frame)
	{
		frame.Clear(); // Resets draw data but keeps internal ImDrawList pools warm
		std::lock_guard lock(s_imguiPoolMutex);
		s_imguiPool.push_back(std::move(frame));
	}

	// ---------------------------------------------------------------------------
	// Lifetime
	// ---------------------------------------------------------------------------

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

	ImguiFrameData::ImguiFrameData(ImguiFrameData&& other) noexcept
	      : m_drawData(other.m_drawData), m_mainPool(std::move(other.m_mainPool)), m_secondaryPool(std::move(other.m_secondaryPool)), m_secondary(std::move(other.m_secondary))
	{
		other.m_drawData.Clear();
	}

	ImguiFrameData& ImguiFrameData::operator=(ImguiFrameData&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}

		for (ImDrawList* list: m_mainPool)
		{
			IM_DELETE(list);
		}
		for (ImDrawList* list: m_secondaryPool)
		{
			IM_DELETE(list);
		}

		m_drawData = other.m_drawData;
		m_mainPool = std::move(other.m_mainPool);
		m_secondaryPool = std::move(other.m_secondaryPool);
		m_secondary = std::move(other.m_secondary);

		other.m_drawData.Clear();
		return *this;
	}

	// ---------------------------------------------------------------------------
	// Clear
	// ---------------------------------------------------------------------------

	void ImguiFrameData::Clear()
	{
		m_drawData.Clear();

		for (auto& vp: m_secondary)
		{
			vp.draw.Clear();
		}

		m_secondary.clear();
	}

	// ---------------------------------------------------------------------------
	// CloneInto
	// ---------------------------------------------------------------------------

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
		// Do not carry ImTextureData* pointers across to the render thread:
		// ImGui owns them in the live context/platform texture list.
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
					// only carry vertex/index/command buffers. Constructing them against
					// the live context's ImDrawListSharedData would register them in its
					// DrawLists registry, and because the frame pool is process-lifetime
					// (kept warm in a static pool), they outlive ImGui::DestroyContext()
					// and trip ~ImDrawListSharedData's `DrawLists.Size == 0` assertion at
					// shutdown. Keeping _Data null decouples the snapshot from the context.
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
			// from the live context's shared data (see the IM_NEW(nullptr) above).
			// The Vulkan backend renders from the buffers only and never reads _Data.

			// NO manual TexRef stripping. The Vulkan backend resolves textures via
			// dst.Textures during RenderDrawData.

			dst.CmdLists[i] = dstList;
			dst.TotalVtxCount += dstList->VtxBuffer.Size;
			dst.TotalIdxCount += dstList->IdxBuffer.Size;
		}
	}

	// ---------------------------------------------------------------------------
	// Capture
	// ---------------------------------------------------------------------------

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
