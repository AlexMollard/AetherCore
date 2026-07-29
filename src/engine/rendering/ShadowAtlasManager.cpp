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
		if (width == 0 || height == 0 || width > kAtlasWidth || height > kAtlasHeight)
		{
			return {};
		}

		for (Shelf& shelf: m_shelves)
		{
			if (shelf.height >= height && shelf.cursorX + width <= kAtlasWidth)
			{
				Region r{.x = shelf.cursorX, .y = shelf.y, .width = width, .height = height};
				shelf.cursorX += width;

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
		}

		const std::uint32_t nextY = m_shelves.empty() ? 0 : (m_shelves.back().y + m_shelves.back().height);
		if (nextY + height > kAtlasHeight)
		{
			return {};
		}

		m_shelves.push_back(Shelf{.y = nextY, .height = height, .cursorX = width});
		Region r{.x = 0, .y = nextY, .width = width, .height = height};

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
