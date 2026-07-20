#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "assets/AssetTypes.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	// Cell encoding: bits 0..15 = palette index + 1 (0 == empty cell),
	// bit 16 = flip X, bit 17 = flip Y. Remaining bits reserved for variation.
	namespace tilecell
	{
		inline constexpr std::uint32_t kEmpty = 0;
		inline constexpr std::uint32_t kIndexMask = 0xFFFFu;
		inline constexpr std::uint32_t kFlipX = 1u << 16u;
		inline constexpr std::uint32_t kFlipY = 1u << 17u;

		[[nodiscard]] constexpr std::uint32_t Make(std::uint16_t paletteIndex, bool flipX = false, bool flipY = false) noexcept
		{
			return (static_cast<std::uint32_t>(paletteIndex) + 1u) | (flipX ? kFlipX : 0u) | (flipY ? kFlipY : 0u);
		}

		[[nodiscard]] constexpr bool Empty(std::uint32_t cell) noexcept
		{
			return (cell & kIndexMask) == 0;
		}

		[[nodiscard]] constexpr std::uint16_t PaletteIndex(std::uint32_t cell) noexcept
		{
			return static_cast<std::uint16_t>((cell & kIndexMask) - 1u);
		}
	} // namespace tilecell

	inline constexpr std::int32_t kTileChunkSize = 32;
	inline constexpr std::size_t kTileChunkCellCount = static_cast<std::size_t>(kTileChunkSize) * static_cast<std::size_t>(kTileChunkSize);

	struct TileChunkKey
	{
		std::int32_t x = 0;
		std::int32_t y = 0;
		bool operator==(const TileChunkKey&) const = default;
	};

	struct TileChunkKeyHash
	{
		[[nodiscard]] std::size_t operator()(const TileChunkKey& key) const noexcept
		{
			return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)) << 32u) | static_cast<std::uint32_t>(key.y));
		}
	};

	struct TileChunk
	{
		std::array<std::uint32_t, kTileChunkCellCount> cells{}; // row-major, kEmpty by default
		// Bumped on every edit; render/collision caches compare against it.
		// In-memory only - never serialized (resets to 1 on load).
		std::uint32_t revision = 1;

		[[nodiscard]] bool IsEmpty() const noexcept;
	};

	struct TileMapLayer
	{
		std::string name = "Layer";
		bool visible = true;
		bool collision = true;
		float opacity = 1.0f;
		glm::vec4 tint{1.0f};
		std::int32_t sortingLayer = 0;
		std::int32_t orderInLayer = 0;
		std::unordered_map<TileChunkKey, TileChunk, TileChunkKeyHash> chunks; // empty chunks omitted
	};

	// Chunked cell data referenced by TileMapComponent; always stored in its own
	// binary file (magic "ATLM", zstd-compressed chunks), never in scene TOML.
	struct TileMapAsset
	{
		static constexpr std::uint32_t kFormatVersion = 1;

		std::string tileSetPath;
		float cellSize = 1.0f; // mirrored from the tileset when bound
		std::vector<AssetObjectId> tilePalette; // cell palette indices point here
		std::vector<TileMapLayer> layers;

		// Palette index for a tile id, appending it when new. Palette entries are
		// never removed so existing cells stay valid.
		[[nodiscard]] std::uint16_t PaletteIndexFor(AssetObjectId tileId);

		// World-cell accessors: chunk-sparse, auto-create on write, auto-prune
		// chunks that become empty. SetCell bumps the chunk revision (identical
		// writes are ignored).
		void SetCell(std::size_t layer, glm::ivec2 cell, std::uint32_t value);
		[[nodiscard]] std::uint32_t GetCell(std::size_t layer, glm::ivec2 cell) const noexcept;

		[[nodiscard]] std::size_t TotalCellCount() const noexcept; // non-empty cells across layers

		[[nodiscard]] Expected<void> Save(const std::filesystem::path& path) const;
		[[nodiscard]] static Expected<TileMapAsset> Load(const std::filesystem::path& path);
	};

	// Cell -> chunk mapping (floor division so negative cells map correctly).
	[[nodiscard]] constexpr std::int32_t TileFloorDiv(std::int32_t value, std::int32_t divisor) noexcept
	{
		return (value >= 0) ? value / divisor : -((-value - 1) / divisor) - 1;
	}

	[[nodiscard]] constexpr TileChunkKey ChunkKeyFor(glm::ivec2 cell) noexcept
	{
		return {TileFloorDiv(cell.x, kTileChunkSize), TileFloorDiv(cell.y, kTileChunkSize)};
	}

	[[nodiscard]] constexpr std::size_t CellIndexInChunk(glm::ivec2 cell) noexcept
	{
		const std::int32_t localX = cell.x - TileFloorDiv(cell.x, kTileChunkSize) * kTileChunkSize;
		const std::int32_t localY = cell.y - TileFloorDiv(cell.y, kTileChunkSize) * kTileChunkSize;
		return static_cast<std::size_t>(localY) * static_cast<std::size_t>(kTileChunkSize) + static_cast<std::size_t>(localX);
	}
} // namespace aether
