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

		using Index = std::unordered_map<std::string, EntryInfo>;

		// Find an entry matching path.  Tries exact lookup first (O(1)), then
		// falls back to case-insensitive comparison (O(n) on miss).
		// If still not found and bestMatch is non-null, runs Levenshtein-based
		// fuzzy search and writes the closest matching path (if any).
		[[nodiscard]] Index::const_iterator FindInsensitive(std::string_view path, std::string* bestMatch = nullptr) const;

		// Collect up to maxSuggestions paths from the index that are
		// Levenshtein-close to path (used for "Did you mean?" messages).
		[[nodiscard]] std::vector<std::string> CollectDidYouMean(std::string_view path, int maxSuggestions = 3) const;

		std::filesystem::path m_pakPath;
		uint64_t m_assetDataBase{0};
		Index m_index;
	};
} // namespace aether::io
