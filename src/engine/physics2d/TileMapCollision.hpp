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

	// One traced outline of a solid region, in chunk-local CELL-CORNER
	// coordinates, wound counter-clockwise (Box2D chain convention: the
	// normal points right of the segment direction, i.e. out of the solid).
	//
	// isLoop: a boundary fully owned by this chunk. Otherwise the path was
	// cut where the surface continues into a neighbouring chunk; both ends
	// carry TWO ghost points extending into the neighbour so Box2D's dead
	// first/final chain edges land on geometry the neighbour owns for real
	// (open chains may overlap on their end points by design).
	struct TileChainPath
	{
		std::vector<glm::ivec2> points;
		bool isLoop = false;
	};

	// Trace the outlines of every solid region with at least one cell in this
	// chunk. `solidAt` takes chunk-local cell coordinates and MUST answer
	// slightly outside [0, kTileChunkSize) too (up to 2 cells) by consulting
	// neighbouring chunks - that is what removes seam ghost collisions.
	[[nodiscard]] std::vector<TileChainPath> TraceSolidOutlines(const std::function<bool(glm::ivec2)>& solidAt);
} // namespace aether
