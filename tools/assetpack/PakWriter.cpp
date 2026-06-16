#include "PakWriter.hpp"

#include "AssetProcessor.hpp"
#include "PakLog.hpp"
#include "PakManifest.hpp"
#include "ThreadPool.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <iomanip>

#include <PakFormat.hpp>

// ---------------------------------------------------------------------------
// PakWriter
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;

void PakWriter::AddDirectory(const fs::path& sourceDir)
{
	m_sourceDir = sourceDir;

	for (const auto& entry: fs::recursive_directory_iterator(sourceDir))
	{
		if (!entry.is_regular_file())
		{
			continue;
		}

		const auto rel = entry.path().lexically_relative(sourceDir);
		const auto vpath = rel.generic_string();
		m_files.push_back({vpath, entry.path()});
	}

	std::sort(m_files.begin(), m_files.end(), [](const FileRecord& a, const FileRecord& b) { return a.virtualPath < b.virtualPath; });
}

bool PakWriter::Write(const fs::path& outPath) const
{
	const auto wallStart = std::chrono::steady_clock::now();
	const auto manifestPath = fs::path(outPath.string() + ".manifest");
	const auto logPath = fs::path(outPath.string() + ".log");

	// --- Incremental check ---------------------------------------------------
	const ManifestMap manifest = LoadManifest(manifestPath);
	if (IsUpToDate(outPath, manifest, m_files))
	{
		std::cout << "AssetPacker: " << outPath.filename().string() << " is up to date, skipping.\n";
		return true;
	}

	// --- Read + compress (parallel) ------------------------------------------
	if (m_compressionLevel > 0)
	{
		std::cout << "AssetPacker: reading and compressing " << m_files.size() << " file(s) (zstd level " << m_compressionLevel << ")...\n";
	}
	else
	{
		std::cout << "AssetPacker: reading " << m_files.size() << " file(s) (compression disabled)...\n";
	}

	std::vector<std::future<PakFileResult>> futures;
	futures.reserve(m_files.size());
	ThreadPool pool;
	for (const auto& file: m_files)
	{
		futures.push_back(pool.submit([virtualPath = file.virtualPath, diskPath = file.diskPath, sourceDir = m_sourceDir, compressionLevel = m_compressionLevel]() { return ProcessFile(virtualPath, diskPath, sourceDir, compressionLevel); }));
	}

	// --- Collect results ----------------------------------------------------
	std::vector<PakFileResult> allResults;
	allResults.reserve(futures.size());
	bool anyError = false;

	for (auto& future: futures)
	{
		PakFileResult res = future.get();
		if (!res.ok)
		{
			std::cerr << "  ! WARNING: " << res.errorMsg << " - skipping\n";
			anyError = true;
		}
		allResults.push_back(std::move(res));
	}

	// --- Flatten primary + extra files, sort by virtual path ----------------
	struct PackItem
	{
		std::string virtualPath;
		std::vector<std::byte> data;
		uint32_t flags = 0;
		uint64_t rawSize = 0;
		uint64_t contentHash = 0;
	};

	std::vector<PackItem> items;
	items.reserve(allResults.size() * 2);

	for (const auto& res: allResults)
	{
		if (!res.ok)
		{
			continue;
		}

		if (res.hasPrimary)
		{
			items.push_back({res.virtualPath, res.data, res.flags, res.rawSize, res.contentHash});
		}

		for (const auto& extra: res.extraFiles)
		{
			if (extra.data.empty())
			{
				continue;
			}
			items.push_back({extra.virtualPath, extra.data, extra.flags, extra.rawSize, extra.contentHash});
		}
	}

	std::sort(items.begin(), items.end(), [](const PackItem& a, const PackItem& b) { return a.virtualPath < b.virtualPath; });

	// --- Build in-memory sections -------------------------------------------
	std::vector<std::byte> pathData;
	std::vector<std::byte> assetData;
	std::vector<PakEntry> entries;
	std::vector<LogEntry> logEntries;
	ManifestMap newManifest;
	entries.reserve(items.size());
	logEntries.reserve(items.size());
	newManifest.reserve(m_files.size() + items.size());

	auto PushPathString = [&](std::string_view s)
	{
		const auto p = reinterpret_cast<const std::byte*>(s.data());
		pathData.insert(pathData.end(), p, p + s.size());
		pathData.push_back(std::byte{0});
	};

	uint64_t totalRawBytes = 0;

	for (const auto& item: items)
	{
		totalRawBytes += item.rawSize;
		const uint64_t onDisk = static_cast<uint64_t>(item.data.size());

		PakEntry entry;
		entry.pathOffset = static_cast<uint32_t>(pathData.size());
		entry.pathLen = static_cast<uint32_t>(item.virtualPath.size());
		entry.flags = item.flags;
		entry.dataOffset = static_cast<uint64_t>(assetData.size());
		entry.dataSize = onDisk;
		entry.contentHash = item.contentHash;
		entries.push_back(entry);

		logEntries.push_back({item.virtualPath, item.rawSize, onDisk, item.contentHash, item.flags});

		PushPathString(item.virtualPath);
		assetData.insert(assetData.end(), item.data.begin(), item.data.end());

		// Console line
		const bool compressed = (item.flags & PAK_FLAG_ZSTD) != 0;
		std::cout << "  + " << std::left << std::setw(52) << item.virtualPath;
		if (compressed)
		{
			const double ratio = 100.0 * (1.0 - static_cast<double>(onDisk) / static_cast<double>(item.rawSize));
			std::cout << FormatSize(item.rawSize) << " -> " << FormatSize(onDisk) << "  (" << std::fixed << std::setprecision(1) << ratio << "% smaller)\n";
		}
		else
		{
			std::cout << FormatSize(item.rawSize) << "  (stored raw)\n";
		}
	}

	// Build manifest from source files (mtime-based) + derived files (hash-only)
	for (std::size_t ri = 0; ri < allResults.size(); ++ri)
	{
		const PakFileResult& res = allResults[ri];
		if (!res.ok)
		{
			continue;
		}

		{
			std::error_code ec;
			const auto mtime = fs::last_write_time(m_files[ri].diskPath, ec);
			const int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
			newManifest[m_files[ri].virtualPath] = {mtimeSec, res.contentHash};
		}

		for (const auto& extra: res.extraFiles)
		{
			if (extra.data.empty())
			{
				continue;
			}
			newManifest[extra.virtualPath] = {0, extra.contentHash};
		}
	}

	// --- Write pak file ------------------------------------------------------
	const uint64_t entryTableSize = sizeof(PakEntry) * entries.size();
	const uint64_t pathDataOffset = sizeof(PakHeader) + entryTableSize;
	const uint64_t assetDataOffset = pathDataOffset + static_cast<uint64_t>(pathData.size());

	PakHeader header;
	header.numEntries = static_cast<uint32_t>(entries.size());
	header.pathDataOffset = pathDataOffset;
	header.pathDataSize = static_cast<uint64_t>(pathData.size());
	header.assetDataOffset = assetDataOffset;
	header.assetDataSize = static_cast<uint64_t>(assetData.size());

	fs::create_directories(outPath.parent_path());

	const auto wallEnd = std::chrono::steady_clock::now();
	const double elapsedSecs = std::chrono::duration<double>(wallEnd - wallStart).count();

	const uint64_t pakBytes = sizeof(PakHeader) + static_cast<uint64_t>(entryTableSize) + static_cast<uint64_t>(pathData.size()) + static_cast<uint64_t>(assetData.size());

	// Write to a temp file first; rename on success for tear-free output
	const fs::path tmpPath = outPath.string() + ".tmp";

	std::cout << "AssetPacker: writing " << outPath.generic_string() << "...\n";

	{
		std::ofstream out(tmpPath, std::ios::binary);
		if (!out)
		{
			std::cerr << "AssetPacker: failed to create output file: " << outPath << "\n";
			return false;
		}

		out.write(reinterpret_cast<const char*>(&header), sizeof(header));
		out.write(reinterpret_cast<const char*>(entries.data()), static_cast<std::streamsize>(entryTableSize));
		out.write(reinterpret_cast<const char*>(pathData.data()), static_cast<std::streamsize>(pathData.size()));
		out.write(reinterpret_cast<const char*>(assetData.data()), static_cast<std::streamsize>(assetData.size()));

		if (!out)
		{
			std::cerr << "AssetPacker: write error on " << outPath << "\n";
			std::error_code ec;
			fs::remove(tmpPath, ec);
			return false;
		}
	}

	if (!anyError)
	{
		std::error_code ec;
		fs::rename(tmpPath, outPath, ec);
		if (ec)
		{
			std::cerr << "AssetPacker: failed to rename temp file to " << outPath << ": " << ec.message() << "\n";
			fs::remove(tmpPath, ec);
			return false;
		}
	}
	else
	{
		std::error_code ec;
		fs::remove(tmpPath, ec);
	}

	SaveManifest(manifestPath, newManifest);
	SaveLog(logPath, m_sourceDir, outPath, m_compressionLevel, logEntries, totalRawBytes, pakBytes, elapsedSecs);

	// --- Console summary -----------------------------------------------------
	const double savings = totalRawBytes > 0 ? 100.0 * (1.0 - static_cast<double>(pakBytes) / static_cast<double>(totalRawBytes)) : 0.0;

	std::cout << "AssetPacker: done.\n";
	std::cout << "  Entries:  " << entries.size() << "\n";
	std::cout << "  Raw size: " << FormatSize(totalRawBytes) << "\n";
	std::cout << "  Pak size: " << FormatSize(pakBytes);
	if (totalRawBytes > 0)
	{
		std::cout << "  (" << std::fixed << std::setprecision(1) << savings << "% smaller)";
	}
	std::cout << "\n";
	std::cout << "  Time:     " << FormatDuration(elapsedSecs) << "\n";
	std::cout << "  Log:      " << logPath.generic_string() << "\n";

	return !anyError;
}
