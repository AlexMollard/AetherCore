#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "IFileBackend.hpp"

namespace aether::io
{
	// On-disk layout (all integers little-endian):
	//
	//   PakHeader                   48 bytes
	//   PakEntry[header.numEntries] 24 bytes each
	//   path-data section           concatenated null-terminated UTF-8 strings
	//   asset-data section          raw file bytes
	//
	// Virtual paths stored in the pak use forward slashes and are relative to
	// the source root that was passed to the AssetPacker tool.
	// Example:  "models/Fox/Fox.gltf"

#pragma pack(push, 1)

	struct PakHeader
	{
		char magic[4];    // "AEPK"
		uint32_t version; // 1
		uint32_t numEntries;
		uint32_t reserved;        // must be 0
		uint64_t pathDataOffset;  // byte offset of path-data section
		uint64_t pathDataSize;    // byte length of path-data section
		uint64_t assetDataOffset; // byte offset of asset-data section
		uint64_t assetDataSize;   // byte length of asset-data section
	};

	static_assert(sizeof(PakHeader) == 48);

	struct PakEntry
	{
		uint32_t pathOffset; // byte offset within path-data section
		uint32_t pathLen;    // character count, NOT including null terminator
		uint64_t dataOffset; // byte offset within asset-data section
		uint64_t dataSize;   // byte count
	};

	static_assert(sizeof(PakEntry) == 24);
#pragma pack(pop)

	// IFileBackend implementation that reads assets from a compiled .pak file.
	class PakBackend final : public IFileBackend
	{
	public:
		// Parses the header and builds an in-memory index on construction.
		// Throws FileSystemError if the file is missing or has an invalid header.
		explicit PakBackend(std::filesystem::path pakPath);

		[[nodiscard]] bool Exists(std::string_view relativePath) const override;
		[[nodiscard]] std::vector<std::byte> Read(std::string_view relativePath) const override;
		[[nodiscard]] std::unique_ptr<std::istream> OpenStream(std::string_view relativePath) const override;
		[[nodiscard]] std::vector<std::string> Glob(std::string_view pattern, const FileGlobOptions& options) const override;

	private:
		struct EntryInfo
		{
			uint64_t offset; // byte offset within asset-data section
			uint64_t size;   // byte count
		};

		std::filesystem::path m_pakPath;
		uint64_t m_assetDataBase{ 0 };
		std::unordered_map<std::string, EntryInfo> m_index;
	};
} // namespace aether::io
