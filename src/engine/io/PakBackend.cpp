#include "PakBackend.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <queue>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <zstd.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "utils/AetherExceptions.hpp"
#include "utils/Expected.hpp"
#include "utils/StringUtils.hpp"

namespace aether::io
{
	namespace
	{
		// Converts a VFS glob pattern (*, ?, **) to a std::regex string.
		// Mirrors the implementation in DirectoryBackend.cpp so the two backends
		// behave identically when callers use Glob().
		std::string GlobToRegex(std::string_view pattern)
		{
			std::string regex;
			regex.reserve(pattern.size() * 2 + 2);
			regex += '^';

			for (std::size_t i = 0; i < pattern.size(); ++i)
			{
				const char c = pattern[i];
				if (c == '*')
				{
					const bool isDoubleStar = (i + 1 < pattern.size() && pattern[i + 1] == '*');
					if (isDoubleStar)
					{
						const bool followedBySlash = (i + 2 < pattern.size() && pattern[i + 2] == '/');
						if (followedBySlash)
						{
							regex += "(?:.*/)?";
							i += 2;
						}
						else
						{
							regex += ".*";
							++i;
						}
					}
					else
					{
						regex += "[^/]*";
					}
					continue;
				}
				if (c == '?')
				{
					regex += "[^/]";
					continue;
				}
				if (c == '.' || c == '^' || c == '$' || c == '+' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|' || c == '\\')
				{
					regex += '\\';
				}
				regex += c;
			}

			regex += '$';
			return regex;
		}
	} // namespace

	PakBackend::PakBackend(std::filesystem::path pakPath)
	      : m_pakPath(std::move(pakPath))
	{
		std::ifstream pak(m_pakPath, std::ios::binary);
		if (!pak)
		{
			throw FileSystemError("Cannot open pak file: " + m_pakPath.string());
		}

		PakHeader header{};
		pak.read(reinterpret_cast<char*>(&header), sizeof(header));

		if (!pak || std::string_view(header.magic, 4) != "AEPK")
		{
			throw FileSystemError("Invalid pak magic in: " + m_pakPath.string());
		}

		if (header.version != PAK_VERSION)
		{
			throw FileSystemError("Unsupported pak version (" + std::to_string(header.version) + ", expected " + std::to_string(PAK_VERSION) + ") in: " + m_pakPath.string());
		}

		// Read entry table.
		std::vector<PakEntry> entries(header.numEntries);
		pak.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(header.numEntries * sizeof(PakEntry)));

		// Read path-data section.
		std::vector<char> pathData(static_cast<std::size_t>(header.pathDataSize));
		pak.seekg(static_cast<std::streamoff>(header.pathDataOffset));
		pak.read(pathData.data(), static_cast<std::streamsize>(header.pathDataSize));

		if (!pak)
		{
			throw FileSystemError("Failed to read pak index from: " + m_pakPath.string());
		}

		m_assetDataBase = header.assetDataOffset;
		m_index.reserve(header.numEntries);

		for (const auto& e: entries)
		{
			std::string path(pathData.data() + e.pathOffset, e.pathLen);
			m_index.emplace(std::move(path), EntryInfo{e.dataOffset, e.dataSize, e.contentHash, e.flags});
		}
	}

	bool PakBackend::Exists(std::string_view relativePath) const
	{
		if (FindInsensitive(relativePath) != m_index.end())
		{
			return true;
		}

		// Also return true if the path is a virtual folder prefix.
		const std::string prefix = std::string(relativePath) + '/';
		for (const auto& [key, _]: m_index)
		{
			if (key.starts_with(prefix))
			{
				return true;
			}
		}
		return false;
	}

	Expected<std::vector<std::byte>> PakBackend::Read(std::string_view relativePath) const
	{
		std::string bestMatch;
		const auto it = FindInsensitive(relativePath, &bestMatch);
		if (it == m_index.end())
		{
			std::string msg = "asset not found in pak: " + std::string(relativePath);
			if (!bestMatch.empty())
			{
				msg += ". Did you mean '" + bestMatch + "'?";
			}
			else
			{
				auto suggestions = CollectDidYouMean(relativePath);
				if (!suggestions.empty())
				{
					msg += ". Did you mean: ";
					for (std::size_t i = 0; i < suggestions.size(); ++i)
					{
						if (i > 0)
						{
							msg += ", ";
						}
						msg += "'" + suggestions[i] + "'";
					}
					msg += "?";
				}
			}
			AE_UNEXPECTED(AetherError::FileSystem(std::move(msg)));
		}

		const auto& info = it->second;

		std::ifstream pak(m_pakPath, std::ios::binary);
		if (!pak)
		{
			AE_UNEXPECTED(AetherError::FileSystem("cannot open pak file for read: " + m_pakPath.string()));
		}

		pak.seekg(static_cast<std::streamoff>(m_assetDataBase + info.offset));

		std::vector<std::byte> onDisk(static_cast<std::size_t>(info.size));
		pak.read(reinterpret_cast<char*>(onDisk.data()), static_cast<std::streamsize>(info.size));

		if (!pak)
		{
			AE_UNEXPECTED(AetherError::FileSystem("read error for asset: " + std::string(relativePath)));
		}

		if (!(info.flags & PAK_FLAG_ZSTD))
		{
			const uint64_t actual = XXH3_64bits(onDisk.data(), onDisk.size());
			if (actual != info.hash)
			{
				AE_UNEXPECTED(AetherError::FileSystem("hash mismatch (corrupt pak data) for asset: " + std::string(relativePath)));
			}
			return onDisk;
		}

		// Decompress: ask the zstd frame header for the original content size.
		const unsigned long long decompSize = ZSTD_getFrameContentSize(onDisk.data(), onDisk.size());
		if (decompSize == ZSTD_CONTENTSIZE_ERROR || decompSize == ZSTD_CONTENTSIZE_UNKNOWN)
		{
			AE_UNEXPECTED(AetherError::FileSystem("corrupt zstd frame for asset: " + std::string(relativePath)));
		}

		std::vector<std::byte> result(static_cast<std::size_t>(decompSize));
		const std::size_t written = ZSTD_decompress(result.data(), result.size(), onDisk.data(), onDisk.size());

		if (ZSTD_isError(written))
		{
			AE_UNEXPECTED(AetherError::FileSystem(std::string("zstd decompress failed for '") + std::string(relativePath) + "': " + ZSTD_getErrorName(written)));
		}

		const uint64_t actual = XXH3_64bits(result.data(), result.size());
		if (actual != info.hash)
		{
			AE_UNEXPECTED(AetherError::FileSystem("hash mismatch (corrupt pak data) for asset: " + std::string(relativePath)));
		}

		return result;
	}

	Expected<std::unique_ptr<std::istream>> PakBackend::OpenStream(std::string_view relativePath) const
	{
		AE_TRY(bytes, Read(relativePath));
		std::string buf(reinterpret_cast<const char*>(bytes->data()), bytes->size());
		return std::make_unique<std::istringstream>(std::move(buf), std::ios::binary);
	}

	Expected<std::vector<std::string>> PakBackend::Glob(std::string_view pattern, const FileGlobOptions& options) const
	{
		const auto regexFlags = options.caseSensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase;

		const std::regex re(GlobToRegex(pattern), regexFlags);

		std::vector<std::string> results;
		for (const auto& [path, info]: m_index)
		{
			if (std::regex_search(path, re))
			{
				results.push_back(path);
			}
		}
		std::sort(results.begin(), results.end());
		return results;
	}

	PakBackend::Index::const_iterator PakBackend::FindInsensitive(std::string_view path, std::string* bestMatch) const
	{
		// Exact match (O(1) fast path).
		auto it = m_index.find(std::string(path));
		if (it != m_index.end())
		{
			return it;
		}

		// Case-insensitive scan (O(n) on miss).
		for (auto ci = m_index.begin(); ci != m_index.end(); ++ci)
		{
			if (utils::IEq(ci->first, path))
			{
				if (bestMatch)
				{
					*bestMatch = ci->first;
				}
				return ci;
			}
		}

		// Fuzzy (Levenshtein) – only on explicit request.
		if (bestMatch)
		{
			*bestMatch = {};
			int bestDist = (std::numeric_limits<int>::max)();
			for (const auto& ci: m_index)
			{
				const int d = utils::Levenshtein(path, ci.first);
				if (d < bestDist && d <= static_cast<int>(path.size()) / 2 + 1)
				{
					bestDist = d;
					*bestMatch = ci.first;
				}
			}
		}

		return m_index.end();
	}

	std::vector<std::string> PakBackend::CollectDidYouMean(std::string_view path, int maxSuggestions) const
	{
		using Pair = std::pair<int, std::string>;
		auto cmp = [](const Pair& a, const Pair& b) { return a.first > b.first; };
		std::priority_queue<Pair, std::vector<Pair>, decltype(cmp)> pq(cmp);

		for (const auto& ci: m_index)
		{
			const int d = utils::Levenshtein(path, ci.first);
			pq.emplace(d, ci.first);
			if (static_cast<int>(pq.size()) > maxSuggestions)
			{
				pq.pop();
			}
		}

		std::vector<std::string> results;
		while (!pq.empty())
		{
			results.push_back(std::move(pq.top().second));
			pq.pop();
		}
		std::reverse(results.begin(), results.end());
		return results;
	}

} // namespace aether::io
