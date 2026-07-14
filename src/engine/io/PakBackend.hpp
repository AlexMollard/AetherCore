#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <PakFormat.hpp>

#include "IFileBackend.hpp"

namespace aether::io
{
	// The on-disk format is defined in include/PakFormat.hpp (shared with AssetPacker).
	class PakBackend final : public IFileBackend
	{
	public:
		// (e.g. tooling that must report the mismatch rather than reject the pak).
		explicit PakBackend(std::filesystem::path pakPath, bool enforceVersion = true);

		[[nodiscard]] std::optional<uint32_t> DeclaredPipelineVersion() const
		{
			return m_declaredPipelineVersion;
		}

		[[nodiscard]] bool Exists(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::byte>> Read(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::string>> Glob(std::string_view pattern, const FileGlobOptions& options) const override;
		[[nodiscard]] Expected<void> Write(std::string_view relativePath, std::span<const std::byte> data) const override;

	private:
		struct EntryInfo
		{
			uint64_t offset;
			uint64_t size; // bytes on disk (compressed size when PAK_FLAG_ZSTD is set)
			uint64_t hash;
			uint32_t flags;
		};

		using Index = std::unordered_map<std::string, EntryInfo>;

		[[nodiscard]] Index::const_iterator FindInsensitive(std::string_view path, std::string* bestMatch = nullptr) const;

		[[nodiscard]] std::vector<std::string> CollectDidYouMean(std::string_view path, int maxSuggestions = 3) const;

		std::filesystem::path m_pakPath;
		uint64_t m_assetDataBase{0};
		Index m_index;
		std::optional<uint32_t> m_declaredPipelineVersion;
	};
} // namespace aether::io
