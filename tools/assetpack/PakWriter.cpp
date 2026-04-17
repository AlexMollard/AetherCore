#include "PakWriter.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void PakWriter::AddDirectory(const fs::path& sourceDir)
{
	for (const auto& entry: fs::recursive_directory_iterator(sourceDir))
	{
		if (!entry.is_regular_file())
		{
			continue;
		}

		const auto rel = entry.path().lexically_relative(sourceDir);
		const auto vpath = rel.generic_string(); // forward slashes on all platforms
		m_files.push_back({ vpath, entry.path() });
	}

	// Sort for deterministic, reproducible output.
	std::sort(m_files.begin(), m_files.end(), [](const FileRecord& a, const FileRecord& b) { return a.virtualPath < b.virtualPath; });
}

bool PakWriter::Write(const fs::path& outPath) const
{
	std::cout << "AssetPacker: packing " << m_files.size() << " file(s)\n";

	// Build path-data and asset-data sections in memory.
	std::vector<char> pathData;
	std::vector<std::byte> assetData;
	std::vector<PakEntry> entries;
	entries.reserve(m_files.size());

	for (const FileRecord& file: m_files)
	{
		std::ifstream in(file.diskPath, std::ios::binary | std::ios::ate);
		if (!in)
		{
			std::cerr << "  WARNING: cannot open '" << file.diskPath << "' - skipping\n";
			continue;
		}

		const auto fileSize = static_cast<uint64_t>(in.tellg());
		const auto dataOffset = static_cast<uint64_t>(assetData.size());
		in.seekg(0);

		assetData.resize(assetData.size() + static_cast<std::size_t>(fileSize));
		in.read(reinterpret_cast<char*>(assetData.data() + dataOffset), static_cast<std::streamsize>(fileSize));

		PakEntry entry;
		entry.pathOffset = static_cast<uint32_t>(pathData.size());
		entry.pathLen = static_cast<uint32_t>(file.virtualPath.size());
		entry.dataOffset = dataOffset;
		entry.dataSize = fileSize;
		entries.push_back(entry);

		pathData.insert(pathData.end(), file.virtualPath.begin(), file.virtualPath.end());
		pathData.push_back('\0');

		std::cout << "  + " << file.virtualPath << " (" << fileSize << " B)\n";
	}

	// Compute section offsets.
	const uint64_t entryTableSize = sizeof(PakEntry) * entries.size();
	const uint64_t pathDataOffset = sizeof(PakHeader) + entryTableSize;
	const uint64_t assetDataOffset = pathDataOffset + static_cast<uint64_t>(pathData.size());

	PakHeader header;
	header.numEntries = static_cast<uint32_t>(entries.size());
	header.pathDataOffset = pathDataOffset;
	header.pathDataSize = static_cast<uint64_t>(pathData.size());
	header.assetDataOffset = assetDataOffset;
	header.assetDataSize = static_cast<uint64_t>(assetData.size());

	// Write output file.
	fs::create_directories(outPath.parent_path());

	std::ofstream out(outPath, std::ios::binary);
	if (!out)
	{
		std::cerr << "AssetPacker: failed to create output file: " << outPath << "\n";
		return false;
	}

	out.write(reinterpret_cast<const char*>(&header), sizeof(header));
	out.write(reinterpret_cast<const char*>(entries.data()), static_cast<std::streamsize>(entryTableSize));
	out.write(pathData.data(), static_cast<std::streamsize>(pathData.size()));
	out.write(reinterpret_cast<const char*>(assetData.data()), static_cast<std::streamsize>(assetData.size()));

	if (!out)
	{
		std::cerr << "AssetPacker: write error on " << outPath << "\n";
		return false;
	}

	const std::size_t totalBytes = sizeof(PakHeader) + static_cast<std::size_t>(entryTableSize) + pathData.size() + assetData.size();

	std::cout << "AssetPacker: wrote " << outPath.generic_string() << " (" << totalBytes << " bytes total, " << entries.size() << " entries)\n";
	return true;
}
