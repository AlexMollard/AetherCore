#include "assets/TileMapAsset.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <span>

#include <zstd.h>

#include "io/FileSystem.hpp"

namespace aether
{
	namespace
	{
		constexpr std::array<char, 4> kMagic{'A', 'T', 'L', 'M'};
		constexpr int kCompressionLevel = 3;
		constexpr std::size_t kChunkByteSize = kTileChunkCellCount * sizeof(std::uint32_t);
		// Hard ceilings so a corrupt header can never drive allocation.
		constexpr std::uint32_t kMaxPaletteEntries = 65535;
		constexpr std::uint32_t kMaxLayers = 256;
		constexpr std::uint32_t kMaxChunksPerLayer = 1u << 20u;
		constexpr std::uint32_t kMaxStringLength = 4096;

		void WriteBytes(std::string& out, const void* data, std::size_t size)
		{
			out.append(static_cast<const char*>(data), size);
		}

		template<typename T>
		void WritePod(std::string& out, const T& value)
		{
			WriteBytes(out, &value, sizeof(T));
		}

		void WriteString(std::string& out, const std::string& value)
		{
			WritePod(out, static_cast<std::uint32_t>(value.size()));
			WriteBytes(out, value.data(), value.size());
		}

		struct Reader
		{
			const char* data = nullptr;
			std::size_t size = 0;
			std::size_t offset = 0;

			[[nodiscard]] bool Read(void* out, std::size_t bytes)
			{
				if (offset + bytes > size)
				{
					return false;
				}
				std::memcpy(out, data + offset, bytes);
				offset += bytes;
				return true;
			}

			template<typename T>
			[[nodiscard]] bool ReadPod(T& out)
			{
				return Read(&out, sizeof(T));
			}

			[[nodiscard]] bool ReadString(std::string& out)
			{
				std::uint32_t length = 0;
				if (!ReadPod(length) || length > kMaxStringLength || offset + length > size)
				{
					return false;
				}
				out.assign(data + offset, length);
				offset += length;
				return true;
			}
		};
	} // namespace

	bool TileChunk::IsEmpty() const noexcept
	{
		return std::ranges::all_of(cells, [](std::uint32_t cell) { return tilecell::Empty(cell); });
	}

	std::uint16_t TileMapAsset::PaletteIndexFor(AssetObjectId tileId)
	{
		const auto it = std::ranges::find(tilePalette, tileId);
		if (it != tilePalette.end())
		{
			return static_cast<std::uint16_t>(std::distance(tilePalette.begin(), it));
		}
		tilePalette.push_back(tileId);
		return static_cast<std::uint16_t>(tilePalette.size() - 1);
	}

	void TileMapAsset::SetCell(std::size_t layer, glm::ivec2 cell, std::uint32_t value)
	{
		if (layer >= layers.size())
		{
			return;
		}
		TileMapLayer& target = layers[layer];
		const TileChunkKey key = ChunkKeyFor(cell);
		const std::size_t index = CellIndexInChunk(cell);

		auto it = target.chunks.find(key);
		if (it == target.chunks.end())
		{
			if (tilecell::Empty(value))
			{
				return; // clearing a cell in an absent chunk is a no-op
			}
			it = target.chunks.emplace(key, TileChunk{}).first;
		}
		TileChunk& chunk = it->second;
		if (chunk.cells[index] == value)
		{
			return;
		}
		chunk.cells[index] = value;
		++chunk.revision;
		dirty = true;
		if (tilecell::Empty(value) && chunk.IsEmpty())
		{
			target.chunks.erase(it); // keep the map sparse
		}
	}

	std::uint32_t TileMapAsset::GetCell(std::size_t layer, glm::ivec2 cell) const noexcept
	{
		if (layer >= layers.size())
		{
			return tilecell::kEmpty;
		}
		const TileMapLayer& target = layers[layer];
		const auto it = target.chunks.find(ChunkKeyFor(cell));
		if (it == target.chunks.end())
		{
			return tilecell::kEmpty;
		}
		return it->second.cells[CellIndexInChunk(cell)];
	}

	std::size_t TileMapAsset::TotalCellCount() const noexcept
	{
		std::size_t count = 0;
		for (const TileMapLayer& layer: layers)
		{
			for (const auto& [key, chunk]: layer.chunks)
			{
				count += static_cast<std::size_t>(std::ranges::count_if(chunk.cells, [](std::uint32_t cell) { return !tilecell::Empty(cell); }));
			}
		}
		return count;
	}

	Expected<void> TileMapAsset::Save(const std::filesystem::path& path) const
	{
		std::string out;
		out.reserve(4096);
		WriteBytes(out, kMagic.data(), kMagic.size());
		WritePod(out, kFormatVersion);
		WriteString(out, tileSetPath);
		WritePod(out, cellSize);
		WritePod(out, static_cast<std::uint32_t>(tilePalette.size()));
		for (const AssetObjectId id: tilePalette)
		{
			WritePod(out, id.value);
		}
		WritePod(out, static_cast<std::uint32_t>(layers.size()));

		std::vector<char> compressed(ZSTD_compressBound(kChunkByteSize));
		for (const TileMapLayer& layer: layers)
		{
			WriteString(out, layer.name);
			WritePod(out, static_cast<std::uint8_t>(layer.visible ? 1 : 0));
			WritePod(out, static_cast<std::uint8_t>(layer.collision ? 1 : 0));
			WritePod(out, layer.opacity);
			WritePod(out, layer.tint);
			WritePod(out, layer.sortingLayer);
			WritePod(out, layer.orderInLayer);
			WritePod(out, static_cast<std::uint32_t>(layer.chunks.size()));
			// Deterministic chunk order for byte-stable saves.
			std::vector<const std::pair<const TileChunkKey, TileChunk>*> ordered;
			ordered.reserve(layer.chunks.size());
			for (const auto& entry: layer.chunks)
			{
				ordered.push_back(&entry);
			}
			std::ranges::sort(ordered, [](const auto* a, const auto* b) { return std::pair{a->first.y, a->first.x} < std::pair{b->first.y, b->first.x}; });
			for (const auto* entry: ordered)
			{
				WritePod(out, entry->first.x);
				WritePod(out, entry->first.y);
				const std::size_t compressedSize = ZSTD_compress(compressed.data(), compressed.size(), entry->second.cells.data(), kChunkByteSize, kCompressionLevel);
				if (ZSTD_isError(compressedSize) != 0u)
				{
					AE_UNEXPECTED(AetherError::Asset(std::format("Tile map chunk ({}, {}) failed to compress: {}", entry->first.x, entry->first.y, ZSTD_getErrorName(compressedSize))));
				}
				WritePod(out, static_cast<std::uint32_t>(compressedSize));
				WriteBytes(out, compressed.data(), compressedSize);
			}
		}

		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			AE_TRY_VOID(io::FileSystem::WriteFile(pathString, std::as_bytes(std::span{out.data(), out.size()})));
		}
		else
		{
			std::error_code ec;
			std::filesystem::create_directories(path.parent_path(), ec);
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file.is_open())
			{
				AE_UNEXPECTED(AetherError::Asset("Failed to open tile map for writing: '" + path.string() + "'."));
			}
			file.write(out.data(), static_cast<std::streamsize>(out.size()));
			if (!file.good())
			{
				AE_UNEXPECTED(AetherError::Asset("Failed to write tile map '" + path.string() + "'."));
			}
		}
		return {};
	}

	Expected<TileMapAsset> TileMapAsset::Load(const std::filesystem::path& path)
	{
		std::string bytes;
		const std::string pathString = path.generic_string();
		if (pathString.contains("://"))
		{
			AE_TRY(data, io::FileSystem::ReadFile(pathString));
			bytes.assign(reinterpret_cast<const char*>(data->data()), data->size());
		}
		else
		{
			std::ifstream file(path, std::ios::binary);
			if (!file.is_open())
			{
				AE_UNEXPECTED(AetherError::Asset("Failed to open tile map '" + path.string() + "'."));
			}
			bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		}

		Reader reader{bytes.data(), bytes.size()};
		std::array<char, 4> magic{};
		if (!reader.Read(magic.data(), magic.size()) || magic != kMagic)
		{
			AE_UNEXPECTED(AetherError::Asset("Tile map '" + path.string() + "' has an invalid header."));
		}
		std::uint32_t version = 0;
		if (!reader.ReadPod(version) || version == 0 || version > kFormatVersion)
		{
			AE_UNEXPECTED(AetherError::Asset(std::format("Tile map '{}' has unsupported format v{} (current v{}).", path.string(), version, kFormatVersion)));
		}

		TileMapAsset map;
		std::uint32_t paletteCount = 0;
		std::uint32_t layerCount = 0;
		if (!reader.ReadString(map.tileSetPath) || !reader.ReadPod(map.cellSize) || !reader.ReadPod(paletteCount) || paletteCount > kMaxPaletteEntries)
		{
			AE_UNEXPECTED(AetherError::Asset("Tile map '" + path.string() + "' is truncated (header)."));
		}
		map.tilePalette.resize(paletteCount);
		for (AssetObjectId& id: map.tilePalette)
		{
			if (!reader.ReadPod(id.value))
			{
				AE_UNEXPECTED(AetherError::Asset("Tile map '" + path.string() + "' is truncated (palette)."));
			}
		}
		if (!reader.ReadPod(layerCount) || layerCount > kMaxLayers)
		{
			AE_UNEXPECTED(AetherError::Asset("Tile map '" + path.string() + "' is truncated (layers)."));
		}
		map.layers.resize(layerCount);
		for (TileMapLayer& layer: map.layers)
		{
			std::uint8_t visible = 1;
			std::uint8_t collision = 1;
			std::uint32_t chunkCount = 0;
			if (!reader.ReadString(layer.name) || !reader.ReadPod(visible) || !reader.ReadPod(collision) || !reader.ReadPod(layer.opacity) || !reader.ReadPod(layer.tint) || !reader.ReadPod(layer.sortingLayer) || !reader.ReadPod(layer.orderInLayer) ||
			        !reader.ReadPod(chunkCount) || chunkCount > kMaxChunksPerLayer)
			{
				AE_UNEXPECTED(AetherError::Asset("Tile map '" + path.string() + "' is truncated (layer header)."));
			}
			layer.visible = visible != 0;
			layer.collision = collision != 0;
			layer.chunks.reserve(chunkCount);
			for (std::uint32_t i = 0; i < chunkCount; ++i)
			{
				TileChunkKey key{};
				std::uint32_t compressedSize = 0;
				if (!reader.ReadPod(key.x) || !reader.ReadPod(key.y) || !reader.ReadPod(compressedSize) || reader.offset + compressedSize > reader.size)
				{
					AE_UNEXPECTED(AetherError::Asset(std::format("Tile map '{}' is truncated (chunk {} of layer '{}').", path.string(), i, layer.name)));
				}
				TileChunk chunk;
				const std::size_t decompressed = ZSTD_decompress(chunk.cells.data(), kChunkByteSize, reader.data + reader.offset, compressedSize);
				reader.offset += compressedSize;
				if (ZSTD_isError(decompressed) != 0u || decompressed != kChunkByteSize)
				{
					AE_UNEXPECTED(AetherError::Asset(std::format("Tile map '{}' chunk ({}, {}) failed to decompress.", path.string(), key.x, key.y)));
				}
				layer.chunks.emplace(key, std::move(chunk));
			}
		}
		return map;
	}
} // namespace aether
