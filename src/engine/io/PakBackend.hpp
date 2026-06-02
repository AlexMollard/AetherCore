#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <PakFormat.hpp>

#include "IFileBackend.hpp"

namespace aether::io
{
	// IFileBackend implementation that reads assets from a compiled .pak file.
	// The on-disk format is defined in include/PakFormat.hpp (shared with AssetPacker).
	//
	// Virtual paths stored in the pak use forward slashes and are relative to
	// the source root that was passed to the AssetPacker tool.
	// Example:  "models/Fox/Fox.gltf"
	class PakBackend final : public IFileBackend
	{
	public:
		// Parses the header and builds an in-memory index on construction.
		// Throws FileSystemError if the file is missing or has an invalid header.
		explicit PakBackend(std::filesystem::path pakPath);

		[[nodiscard]] bool Exists(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::byte>> Read(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::string>> Glob(std::string_view pattern, const FileGlobOptions& options) const override;

	private:
		struct EntryInfo
		{
			uint64_t offset; // byte offset within asset-data section
			uint64_t size;   // bytes on disk (compressed size when PAK_FLAG_ZSTD is set)
			uint64_t hash;   // XXH3-64 of uncompressed content, verified after read
			uint32_t flags;
		};

		std::filesystem::path m_pakPath;
		uint64_t m_assetDataBase{0};
		std::unordered_map<std::string, EntryInfo> m_index;
	};
} // namespace aether::io
