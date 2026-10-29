#include "rendering/ShadowAtlasManager.hpp"

#include "utils/Profiler.hpp"

namespace aether
{
	void ShadowAtlasManager::Reset()
	{
		AE_PROFILE_ZONE();
		m_shelves.clear();
		m_usedBounds = {};
	}

	ShadowAtlasManager::Region ShadowAtlasManager::Allocate(const std::uint32_t width, const std::uint32_t height)
	{
		AE_PROFILE_ZONE();
		const auto packed = AllocateShadowShelf(m_shelves, kAtlasWidth, kAtlasHeight, width, height);
		Region r{.x = packed.x, .y = packed.y, .width = packed.z, .height = packed.w};
		if (!r.IsValid()) { return {}; }

		if (m_usedBounds.width == 0)
		{
			m_usedBounds = r;
		}
		else
		{
			const std::uint32_t minX = std::min(m_usedBounds.x, r.x);
			const std::uint32_t minY = std::min(m_usedBounds.y, r.y);
			const std::uint32_t maxX = std::max(m_usedBounds.x + m_usedBounds.width, r.x + r.width);
			const std::uint32_t maxY = std::max(m_usedBounds.y + m_usedBounds.height, r.y + r.height);
			m_usedBounds = {.x = minX, .y = minY, .width = maxX - minX, .height = maxY - minY};
		}

		return r;
	}
} // namespace aether
