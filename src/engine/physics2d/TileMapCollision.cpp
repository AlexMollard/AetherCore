#include "physics2d/TileMapCollision.hpp"

#include <bitset>
#include <cstdlib>
#include <optional>
#include <unordered_set>
#include <utility>

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

	namespace
	{
		// A directed boundary edge runs from a cell corner one step along `dir`
		// with solid on its LEFT and empty on its RIGHT - walking every edge
		// this way yields counter-clockwise outlines (Box2D chain winding).
		constexpr std::array<glm::ivec2, 4> kDirs{glm::ivec2{1, 0}, glm::ivec2{0, 1}, glm::ivec2{-1, 0}, glm::ivec2{0, -1}};

		glm::ivec2 LeftCellOf(glm::ivec2 v, glm::ivec2 dir)
		{
			if (dir.x == 1)
			{
				return {v.x, v.y};
			}
			if (dir.x == -1)
			{
				return {v.x - 1, v.y - 1};
			}
			if (dir.y == 1)
			{
				return {v.x - 1, v.y};
			}
			return {v.x, v.y - 1};
		}

		glm::ivec2 RightCellOf(glm::ivec2 v, glm::ivec2 dir)
		{
			if (dir.x == 1)
			{
				return {v.x, v.y - 1};
			}
			if (dir.x == -1)
			{
				return {v.x - 1, v.y};
			}
			if (dir.y == 1)
			{
				return {v.x, v.y};
			}
			return {v.x - 1, v.y - 1};
		}

		std::uint32_t EdgeKey(glm::ivec2 v, glm::ivec2 dir)
		{
			const std::uint32_t dirIndex = dir.x == 1 ? 0u : dir.y == 1 ? 1u : dir.x == -1 ? 2u : 3u;
			// Vertices stay within [-2, kTileChunkSize + 2]; bias well clear.
			return (static_cast<std::uint32_t>(v.x + 8) << 16u) | (static_cast<std::uint32_t>(v.y + 8) << 8u) | dirIndex;
		}

		bool InChunk(glm::ivec2 cell)
		{
			return cell.x >= 0 && cell.x < kTileChunkSize && cell.y >= 0 && cell.y < kTileChunkSize;
		}

		struct OutlineWalker
		{
			const std::function<bool(glm::ivec2)>& solidAt;

			[[nodiscard]] bool EdgeExists(glm::ivec2 v, glm::ivec2 dir) const
			{
				return solidAt(LeftCellOf(v, dir)) && !solidAt(RightCellOf(v, dir));
			}

			// The forward continuation at `v` arriving along `incoming`: prefer
			// the left turn, then straight, then right - hugging the solid
			// region so checkerboard corners never produce crossing outlines.
			[[nodiscard]] std::optional<glm::ivec2> NextDir(glm::ivec2 v, glm::ivec2 incoming) const
			{
				const glm::ivec2 left{-incoming.y, incoming.x};
				const glm::ivec2 right{incoming.y, -incoming.x};
				for (const glm::ivec2 candidate: {left, incoming, right})
				{
					if (EdgeExists(v, candidate))
					{
						return candidate;
					}
				}
				return std::nullopt;
			}

			// The edge whose forward continuation is (v, outgoing) - used to
			// walk a cut path back to its true start deterministically.
			[[nodiscard]] std::optional<glm::ivec2> PrevDir(glm::ivec2 v, glm::ivec2 outgoing) const
			{
				for (const glm::ivec2 candidate: kDirs)
				{
					const glm::ivec2 from = v - candidate;
					if (!EdgeExists(from, candidate))
					{
						continue;
					}
					if (const auto next = NextDir(v, candidate); next && *next == outgoing)
					{
						return candidate;
					}
				}
				return std::nullopt;
			}
		};
	} // namespace

	std::vector<TileChainPath> TraceSolidOutlines(const std::function<bool(glm::ivec2)>& solidAt)
	{
		const OutlineWalker walker{solidAt};
		std::vector<TileChainPath> paths;
		std::unordered_set<std::uint32_t> visited;

		const auto mergeCollinear = [](std::vector<glm::ivec2>& points, std::size_t first, std::size_t last)
		{
			// Drop interior points whose neighbours are collinear, within
			// [first, last] only (ghost end points must stay distinct).
			for (std::size_t i = last - 1; i > first; --i)
			{
				const glm::ivec2 a = points[i - 1];
				const glm::ivec2 b = points[i];
				const glm::ivec2 c = points[i + 1];
				if ((b.x - a.x) * (c.y - b.y) == (b.y - a.y) * (c.x - b.x))
				{
					points.erase(points.begin() + static_cast<std::ptrdiff_t>(i));
				}
			}
		};

		for (std::int32_t y = 0; y < kTileChunkSize; ++y)
		{
			for (std::int32_t x = 0; x < kTileChunkSize; ++x)
			{
				if (!solidAt({x, y}))
				{
					continue;
				}
				// Boundary edges owned by this cell (side neighbour empty).
				const std::array<std::pair<glm::ivec2, glm::ivec2>, 4> seeds{{
				        {{x, y}, {1, 0}},          // bottom side
				        {{x + 1, y}, {0, 1}},      // right side
				        {{x + 1, y + 1}, {-1, 0}}, // top side
				        {{x, y + 1}, {0, -1}},     // left side
				}};
				for (const auto& [seedVertex, seedDir]: seeds)
				{
					if (!walker.EdgeExists(seedVertex, seedDir) || visited.contains(EdgeKey(seedVertex, seedDir)))
					{
						continue;
					}

					// Walk backward to the path's true start: either the loop
					// closes or the surface continues into a neighbour chunk.
					glm::ivec2 startVertex = seedVertex;
					glm::ivec2 startDir = seedDir;
					bool isLoop = false;
					while (true)
					{
						const auto prev = walker.PrevDir(startVertex, startDir);
						if (!prev)
						{
							break;
						}
						const glm::ivec2 prevVertex = startVertex - *prev;
						if (!InChunk(LeftCellOf(prevVertex, *prev)))
						{
							break; // cut: the neighbour owns the previous edge
						}
						if (prevVertex == seedVertex && *prev == seedDir)
						{
							isLoop = true;
							break;
						}
						startVertex = prevVertex;
						startDir = *prev;
					}

					// Walk forward from the start, marking every real edge.
					TileChainPath path;
					path.isLoop = isLoop;
					path.points.push_back(startVertex);
					glm::ivec2 vertex = startVertex;
					glm::ivec2 dir = startDir;
					while (true)
					{
						visited.insert(EdgeKey(vertex, dir));
						vertex += dir;
						path.points.push_back(vertex);
						const auto next = walker.NextDir(vertex, dir);
						if (!next)
						{
							break;
						}
						if (isLoop && vertex == startVertex && *next == startDir)
						{
							break;
						}
						if (!InChunk(LeftCellOf(vertex, *next)))
						{
							break; // cut: the neighbour owns the next edge
						}
						dir = *next;
					}

					if (path.isLoop)
					{
						path.points.pop_back(); // the closing vertex repeats the start
						mergeCollinear(path.points, 0, path.points.size() - 1);
						// A loop's first/last pair may also be collinear across the seam.
						while (path.points.size() > 4)
						{
							const glm::ivec2 a = path.points[path.points.size() - 2];
							const glm::ivec2 b = path.points.back();
							const glm::ivec2 c = path.points.front();
							const glm::ivec2 d = path.points[1];
							if ((b.x - a.x) * (c.y - b.y) == (b.y - a.y) * (c.x - b.x))
							{
								path.points.pop_back();
							}
							else if ((c.x - b.x) * (d.y - c.y) == (c.y - b.y) * (d.x - c.x))
							{
								path.points.erase(path.points.begin());
							}
							else
							{
								break;
							}
						}
					}
					else
					{
						mergeCollinear(path.points, 0, path.points.size() - 1);
						// Ghost extensions: two extra edges past each cut, owned
						// by the neighbour, so Box2D's dead first/final edges sit
						// on geometry the neighbour provides for real.
						glm::ivec2 endVertex = path.points.back();
						glm::ivec2 endDir = endVertex - path.points[path.points.size() - 2];
						endDir = {endDir.x == 0 ? 0 : endDir.x / std::abs(endDir.x), endDir.y == 0 ? 0 : endDir.y / std::abs(endDir.y)};
						for (int i = 0; i < 2; ++i)
						{
							const auto next = walker.NextDir(endVertex, endDir);
							if (!next)
							{
								break;
							}
							endDir = *next;
							endVertex += endDir;
							path.points.push_back(endVertex);
						}
						glm::ivec2 headVertex = path.points.front();
						glm::ivec2 headDir = path.points[1] - headVertex;
						headDir = {headDir.x == 0 ? 0 : headDir.x / std::abs(headDir.x), headDir.y == 0 ? 0 : headDir.y / std::abs(headDir.y)};
						for (int i = 0; i < 2; ++i)
						{
							const auto prev = walker.PrevDir(headVertex, headDir);
							if (!prev)
							{
								break;
							}
							headDir = *prev;
							headVertex -= headDir;
							path.points.insert(path.points.begin(), headVertex);
						}
					}

					if (path.points.size() >= 4)
					{
						paths.push_back(std::move(path));
					}
				}
			}
		}
		return paths;
	}
} // namespace aether
