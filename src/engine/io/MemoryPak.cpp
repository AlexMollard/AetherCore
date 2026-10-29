#include "MemoryPak.hpp"

#include <bit>
#include <algorithm>
#include <cstring>
#include <zstd.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "utils/Utf8.hpp"

namespace aether::io
{
	namespace
	{
		bool ValidPath(std::string_view path)
		{
			if (path.empty() || path.size() > 4096 || path.front() == '/' || path.back() == '/' || path.find_first_of("\\:") != std::string_view::npos || path.find('\0') != std::string_view::npos || !IsValidUtf8(path.data(), path.size()))
			{
				return false;
			}
			while (!path.empty())
			{
				const auto slash = path.find('/');
				const auto part = path.substr(0, slash);
				if (part.empty() || part == "." || part == "..")
				{
					return false;
				}
				if (slash == std::string_view::npos)
				{
					break;
				}
				path.remove_prefix(slash + 1);
			}
			return true;
		}
	} // namespace

	Expected<MemoryPak> MemoryPak::Open(std::span<const std::byte> bytes, std::string_view expectedTarget)
	{
		static_assert(std::endian::native == std::endian::little);
		if (bytes.size() < sizeof(PakHeader) || bytes.size() > kMaxPackageBytes || expectedTarget.empty() || expectedTarget.find_first_of("\r\n=") != std::string_view::npos)
		{
			AE_UNEXPECTED(AetherError::FileSystem("Invalid package size or target"));
		}
		PakHeader header;
		std::memcpy(&header, bytes.data(), sizeof(header));
		if (std::memcmp(header.magic, "AEPK", 4) != 0 || header.version != PAK_VERSION || header.flags != 0 || header.headerReserved != 0 || header.numEntries > 262144)
		{
			AE_UNEXPECTED(AetherError::FileSystem("Unsupported or corrupt package header"));
		}
		const uint64_t tableBytes = uint64_t(header.numEntries) * sizeof(PakEntry);
		const uint64_t pathOffset = sizeof(PakHeader) + tableBytes;
		if (pathOffset > bytes.size() || header.pathDataOffset != pathOffset || header.pathDataSize > bytes.size() - pathOffset || header.assetDataOffset != pathOffset + header.pathDataSize
		        || header.assetDataSize != bytes.size() - header.assetDataOffset)
		{
			AE_UNEXPECTED(AetherError::FileSystem("Corrupt package index bounds"));
		}
		const auto indexBytes = bytes.subspan(sizeof(PakHeader), static_cast<size_t>(tableBytes + header.pathDataSize));
		if (XXH3_64bits(indexBytes.data(), indexBytes.size()) != header.indexHash)
		{
			AE_UNEXPECTED(AetherError::FileSystem("Corrupt package index hash"));
		}
		MemoryPak result;
		result.m_asset_offset = static_cast<size_t>(header.assetDataOffset);
		for (size_t i = 0; i < header.numEntries; ++i)
		{
			PakEntry entry;
			std::memcpy(&entry, bytes.data() + sizeof(PakHeader) + i * sizeof(PakEntry), sizeof(entry));
			if (entry.pathOffset > header.pathDataSize || entry.pathLen > header.pathDataSize - entry.pathOffset || entry.dataOffset > header.assetDataSize || entry.dataSize > header.assetDataSize - entry.dataOffset
			        || (entry.flags & ~PAK_FLAG_ZSTD) != 0 || entry._pad != 0 || entry.dataSize > kMaxAssetBytes)
			{
				AE_UNEXPECTED(AetherError::FileSystem("Corrupt package entry bounds or flags"));
			}
			const std::string path(reinterpret_cast<const char*>(bytes.data() + pathOffset + entry.pathOffset), entry.pathLen);
			if (!ValidPath(path) || !result.m_index.emplace(path, entry).second)
			{
				AE_UNEXPECTED(AetherError::FileSystem("Invalid UTF-8, unsafe or duplicate package path"));
			}
		}
		result.m_bytes.assign(bytes.begin(), bytes.end());
		AE_TRY(manifest, result.Read(PAK_MANIFEST_PATH));
		const std::string text(reinterpret_cast<const char*>(manifest->data()), manifest->size());
		// Match complete fields; substrings and duplicate fields must not qualify a package.
		const auto field = [&text](std::string_view key, std::string_view value)
		{
			const std::string prefix = "\n" + std::string(key) + "=";
			const std::string document = "\n" + text;
			const auto start = document.find(prefix);
			return start != std::string::npos && document.find(prefix, start + 1) == std::string::npos && document.substr(start + prefix.size(), document.find('\n', start + prefix.size()) - start - prefix.size()) == value;
		};
		if (!field("format", "AetherPakManifest") || !field("pakVersion", std::to_string(PAK_VERSION)) || !field("pipelineVersion", std::to_string(PAK_PIPELINE_VERSION)) || !field("target", expectedTarget))
		{
			AE_UNEXPECTED(AetherError::FileSystem("Package target or pipeline version mismatch; rebuild assets"));
		}
		return result;
	}

	Expected<std::vector<std::byte>> MemoryPak::Read(std::string_view path, const std::function<bool(std::size_t)>& reserve) const
	{
		const auto found = m_index.find(std::string(path));
		if (found == m_index.end())
		{
			AE_UNEXPECTED(AetherError::FileSystem("Asset not found in package: " + std::string(path)));
		}
		const auto& entry = found->second;
		const auto data = std::span(m_bytes).subspan(m_asset_offset + static_cast<size_t>(entry.dataOffset), static_cast<size_t>(entry.dataSize));
		std::vector<std::byte> output;
		if ((entry.flags & PAK_FLAG_ZSTD) != 0)
		{
			const auto size = ZSTD_getFrameContentSize(data.data(), data.size());
			if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > kMaxAssetBytes)
			{
				AE_UNEXPECTED(AetherError::FileSystem("Invalid or oversized compressed asset: " + std::string(path)));
			}
			if (reserve && !reserve(static_cast<size_t>(size)))
			{
				AE_UNEXPECTED(AetherError::FileSystem("Asset allocation reservation rejected: " + std::string(path)));
			}
			output.resize(static_cast<size_t>(size));
			const auto decoded = ZSTD_decompress(output.data(), output.size(), data.data(), data.size());
			if (ZSTD_isError(decoded) || decoded != output.size())
			{
				AE_UNEXPECTED(AetherError::FileSystem("Cannot decompress asset: " + std::string(path)));
			}
		}
		else
		{
			if (reserve && !reserve(data.size()))
			{
				AE_UNEXPECTED(AetherError::FileSystem("Asset allocation reservation rejected: " + std::string(path)));
			}
			output.assign(data.begin(), data.end());
		}
		if (XXH3_64bits(output.data(), output.size()) != entry.contentHash)
		{
			AE_UNEXPECTED(AetherError::FileSystem("Corrupt asset hash: " + std::string(path)));
		}
		return output;
	}

	bool MemoryPak::Exists(std::string_view path) const
	{
		return m_index.contains(std::string(path));
	}

	std::vector<std::string> MemoryPak::Paths() const
	{
		std::vector<std::string> paths;
		paths.reserve(m_index.size());
		for (const auto& [path, entry]: m_index)
		{
			paths.push_back(path);
		}
		std::ranges::sort(paths);
		return paths;
	}
} // namespace aether::io
