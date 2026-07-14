#pragma once

#include <cstdint>

// On-disk structures for the .pak binary format.

inline constexpr uint32_t PAK_VERSION = 2;
inline constexpr uint32_t PAK_FLAG_ZSTD = 1u << 0;
inline constexpr uint32_t PAK_PIPELINE_VERSION = 6;
inline constexpr const char* PAK_MANIFEST_PATH = "__aetherpak/manifest.txt";

#pragma pack(push, 1)

struct PakHeader
{
	char magic[4] = {'A', 'E', 'P', 'K'};
	uint32_t version = PAK_VERSION;
	uint32_t numEntries = 0;
	uint32_t flags = 0; // pak-level flags (reserved, must be 0 for now)
	uint64_t pathDataOffset = 0;
	uint64_t pathDataSize = 0;
	uint64_t assetDataOffset = 0;
	uint64_t assetDataSize = 0;
	uint64_t indexHash = 0;
	uint64_t headerReserved = 0; // reserved, must be 0
};

static_assert(sizeof(PakHeader) == 64);

struct PakEntry
{
	uint32_t pathOffset = 0;
	uint32_t pathLen = 0;
	uint32_t flags = 0;
	uint32_t _pad = 0; // reserved, must be 0
	uint64_t dataOffset = 0;
	uint64_t dataSize = 0; // bytes on disk (compressed size when PAK_FLAG_ZSTD is set)
	uint64_t contentHash = 0;
};

static_assert(sizeof(PakEntry) == 40);

#pragma pack(pop)
