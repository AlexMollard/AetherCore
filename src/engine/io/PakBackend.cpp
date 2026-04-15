#include "PakBackend.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "AetherExceptions.hpp"

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
		if (header.version != 1)
		{
			throw FileSystemError("Unsupported pak version (" + std::to_string(header.version) + ") in: " + m_pakPath.string());
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
			m_index.emplace(std::move(path), EntryInfo{ e.dataOffset, e.dataSize });
		}
	}

	bool PakBackend::Exists(std::string_view relativePath) const
	{
		return m_index.contains(std::string(relativePath));
	}

	std::vector<std::byte> PakBackend::Read(std::string_view relativePath) const
	{
		const auto it = m_index.find(std::string(relativePath));
		if (it == m_index.end())
		{
			throw FileSystemError("Asset not found in pak: " + std::string(relativePath));
		}

		const auto& info = it->second;

		std::ifstream pak(m_pakPath, std::ios::binary);
		if (!pak)
		{
			throw FileSystemError("Cannot open pak file for read: " + m_pakPath.string());
		}

		pak.seekg(static_cast<std::streamoff>(m_assetDataBase + info.offset));

		std::vector<std::byte> buffer(static_cast<std::size_t>(info.size));
		pak.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(info.size));

		if (!pak)
		{
			throw FileSystemError("Read error for asset: " + std::string(relativePath));
		}

		return buffer;
	}

	std::unique_ptr<std::istream> PakBackend::OpenStream(std::string_view relativePath) const
	{
		auto bytes = Read(relativePath);
		// Copy into a string so istringstream owns the buffer.
		std::string buf(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return std::make_unique<std::istringstream>(std::move(buf), std::ios::binary);
	}

	std::vector<std::string> PakBackend::Glob(std::string_view pattern, const FileGlobOptions& options) const
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

} // namespace aether::io
