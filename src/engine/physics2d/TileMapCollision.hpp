#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "assets/TileMapAsset.hpp"

namespace aether
{
	struct TileRect
	{
		std::int32_t x = 0;
		std::int32_t y = 0;
		std::int32_t w = 0;
		std::int32_t h = 0;
		bool operator==(const TileRect&) const = default;
	};

	// Greedy meshing of solid cells within one chunk (row-expand then
	// column-expand): deterministic, covers every solid cell exactly once.
	// Coordinates are chunk-local cell units.
	[[nodiscard]] std::vector<TileRect> MergeSolidCells(const std::array<std::uint32_t, kTileChunkCellCount>& cells, const std::function<bool(std::uint32_t cell)>& isSolid);
} // namespace aether
