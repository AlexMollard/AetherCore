#pragma once

#include <cassert>
#include <cstdint>

// On-disk structures for the .pak binary format.
// Layout must exactly match PakBackend.hpp in the engine.
//
//   PakHeader        (48 bytes)
//   PakEntry[N]      (24 bytes each)
//   path-data        concatenated null-terminated UTF-8 strings
//   asset-data       raw file bytes
//
// All integers are little-endian.

#pragma pack(push, 1)

struct PakHeader
{
	char magic[4] = { 'A', 'E', 'P', 'K' };
	uint32_t version = 1;
	uint32_t numEntries = 0;
	uint32_t reserved = 0;
	uint64_t pathDataOffset = 0;
	uint64_t pathDataSize = 0;
	uint64_t assetDataOffset = 0;
	uint64_t assetDataSize = 0;
};

static_assert(sizeof(PakHeader) == 48);

struct PakEntry
{
	uint32_t pathOffset = 0; // byte offset within path-data section
	uint32_t pathLen = 0;    // character count, NOT including null terminator
	uint64_t dataOffset = 0; // byte offset within asset-data section
	uint64_t dataSize = 0;   // byte count
};

static_assert(sizeof(PakEntry) == 24);

#pragma pack(pop)
