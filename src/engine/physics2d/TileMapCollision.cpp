#include "physics2d/TileMapCollision.hpp"

#include <bitset>

namespace aether
{
	std::vector<TileRect> MergeSolidCells(const std::array<std::uint32_t, kTileChunkCellCount>& cells, const std::function<bool(std::uint32_t cell)>& isSolid)
	{
		std::bitset<kTileChunkCellCount> consumed;
		const auto solidAt = [&](std::int32_t x, std::int32_t y)
		{
			const std::size_t index = static_cast<std::size_t>(y) * kTileChunkSize + static_cast<std::size_t>(x);
			return !consumed[index] && isSolid(cells[index]);
		};

		std::vector<TileRect> rects;
		for (std::int32_t y = 0; y < kTileChunkSize; ++y)
		{
			for (std::int32_t x = 0; x < kTileChunkSize; ++x)
			{
				if (!solidAt(x, y))
				{
					continue;
				}
				// Expand right along the row.
				std::int32_t width = 1;
				while (x + width < kTileChunkSize && solidAt(x + width, y))
				{
					++width;
				}
				// Expand down while the full row span stays solid.
				std::int32_t height = 1;
				bool grow = true;
				while (grow && y + height < kTileChunkSize)
				{
					for (std::int32_t dx = 0; dx < width; ++dx)
					{
						if (!solidAt(x + dx, y + height))
						{
							grow = false;
							break;
						}
					}
					if (grow)
					{
						++height;
					}
				}
				for (std::int32_t dy = 0; dy < height; ++dy)
				{
					for (std::int32_t dx = 0; dx < width; ++dx)
					{
						consumed.set(static_cast<std::size_t>(y + dy) * kTileChunkSize + static_cast<std::size_t>(x + dx));
					}
				}
				rects.push_back({x, y, width, height});
			}
		}
		return rects;
	}
} // namespace aether
