#include "PakWriter.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <zstd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
	constexpr std::size_t kMinCompressSize = 64;

	std::string FormatSize(uint64_t bytes)
	{
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(1);
		if (bytes >= 1024ULL * 1024 * 1024)
			ss << static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0) << " GB";
		else if (bytes >= 1024ULL * 1024)
			ss << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
		else if (bytes >= 1024ULL)
			ss << static_cast<double>(bytes) / 1024.0 << " KB";
		else
			ss << bytes << " B";
		return ss.str();
	}

	std::string FormatDuration(double seconds)
	{
		std::ostringstream ss;
		ss << std::fixed << std::setprecision(2) << seconds << " s";
		return ss.str();
	}

	std::string FormatTimestamp()
	{
		const auto now   = std::chrono::system_clock::now();
		const std::time_t tt = std::chrono::system_clock::to_time_t(now);
		std::tm tm{};
#if defined(_WIN32)
		localtime_s(&tm, &tt);
#else
		localtime_r(&tt, &tm);
#endif
		std::ostringstream ss;
		ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
		return ss.str();
	}

	bool IsAlreadyCompressed(const fs::path& path)
	{
		const auto ext = path.extension().string();
		std::string lower;
		lower.reserve(ext.size());
		for (const char c : ext)
			lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

		static constexpr std::string_view kSkip[] = {
		    ".jpg", ".jpeg", ".png", ".webp",
		    ".dds", ".ktx", ".ktx2", ".basis",
		    ".ogg", ".mp3", ".opus", ".flac", ".aac",
		    ".zip", ".gz", ".br", ".zst",
		};
		for (const auto& s : kSkip)
			if (lower == s)
				return true;
		return false;
	}

	// -------------------------------------------------------------------------
	// Manifest — persists per-file mtime + hash so unchanged assets are skipped
	// -------------------------------------------------------------------------

	struct ManifestEntry
	{
		int64_t  mtimeTicks;  // last_write_time ticks at time of packing
		uint64_t contentHash; // XXH3-64 of uncompressed content
	};

	using ManifestMap = std::unordered_map<std::string, ManifestEntry>;

	ManifestMap LoadManifest(const fs::path& manifestPath)
	{
		ManifestMap map;
		std::ifstream in(manifestPath);
		if (!in)
			return map;

		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty() || line[0] == '#')
				continue;

			// Format: virtualPath\tmtimeTicks\thashHex
			const auto t1 = line.find('\t');
			const auto t2 = line.find('\t', t1 + 1);
			if (t1 == std::string::npos || t2 == std::string::npos)
				continue;

			const std::string  vpath    = line.substr(0, t1);
			const std::string_view mstr = { line.data() + t1 + 1, t2 - t1 - 1 };
			const std::string_view hstr = { line.data() + t2 + 1, line.size() - t2 - 1 };

			ManifestEntry e{};
			std::from_chars(mstr.data(), mstr.data() + mstr.size(), e.mtimeTicks);
			std::from_chars(hstr.data(), hstr.data() + hstr.size(), e.contentHash, 16);
			map.emplace(vpath, e);
		}
		return map;
	}

	void SaveManifest(const fs::path& manifestPath, const ManifestMap& map)
	{
		std::ofstream out(manifestPath);
		if (!out)
			return;

		out << "# AetherPak manifest\n";
		for (const auto& [vpath, e] : map)
			out << vpath << '\t' << e.mtimeTicks << '\t' << std::hex << e.contentHash << '\n';
	}

	// Returns true if the pak file exists and every source file's mtime matches
	// the manifest - meaning nothing has changed since the last pack.
	bool IsUpToDate(
	    const fs::path&                          pakPath,
	    const ManifestMap&                       manifest,
	    const std::vector<PakWriter::FileRecord>& files)
	{
		if (!fs::exists(pakPath))
			return false;
		if (manifest.size() != files.size())
			return false;

		for (const auto& file : files)
		{
			const auto it = manifest.find(file.virtualPath);
			if (it == manifest.end())
				return false;

			std::error_code ec;
			const auto mtime = fs::last_write_time(file.diskPath, ec);
			if (ec)
				return false;

			if (mtime.time_since_epoch().count() != it->second.mtimeTicks)
				return false;
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// Log — human-readable build record written to <output>.pak.log
	// -------------------------------------------------------------------------

	struct LogEntry
	{
		std::string virtualPath;
		uint64_t    rawSize    = 0;
		uint64_t    onDiskSize = 0;
		uint64_t    hash       = 0;
		uint32_t    flags      = 0;
	};

	void SaveLog(
	    const fs::path&           logPath,
	    const fs::path&           sourceDir,
	    const fs::path&           pakPath,
	    int                       compressionLevel,
	    const std::vector<LogEntry>& entries,
	    uint64_t                  totalRawBytes,
	    uint64_t                  pakBytes,
	    double                    elapsedSecs)
	{
		std::ofstream out(logPath);
		if (!out)
			return;

		// Header
		out << "AetherPak Build Log\n";
		out << "Generated   : " << FormatTimestamp() << "\n";
		out << "Source      : " << sourceDir.generic_string() << "\n";
		out << "Output      : " << pakPath.generic_string() << "\n";
		out << "Compression : " << (compressionLevel > 0 ? "zstd level " + std::to_string(compressionLevel) : "disabled") << "\n";
		out << "\n";

		// Column widths
		constexpr int kNameW    = 56;
		constexpr int kSizeW    = 10;
		constexpr int kSavingsW = 9;
		constexpr int kHashW    = 18;
		const int     kLineW    = kNameW + kSizeW + kSizeW + kSavingsW + kHashW + 4;

		const std::string rule(kLineW, '-');

		out << std::left  << std::setw(kNameW)    << "  File"
		    << std::right << std::setw(kSizeW)     << "Raw"
		    << std::setw(kSizeW)                   << "Pak"
		    << std::setw(kSavingsW)                << "Savings"
		    << "  " << "Hash"
		    << "\n" << rule << "\n";

		for (const auto& e : entries)
		{
			const bool   compressed = (e.flags & PAK_FLAG_ZSTD) != 0;
			const double ratio      = (compressed && e.rawSize > 0)
			    ? 100.0 * (1.0 - static_cast<double>(e.onDiskSize) / static_cast<double>(e.rawSize))
			    : 0.0;

			std::ostringstream hashStr;
			hashStr << std::hex << std::setfill('0') << std::setw(16) << e.hash;

			std::ostringstream savingsStr;
			if (compressed)
				savingsStr << std::fixed << std::setprecision(1) << ratio << "%";
			else
				savingsStr << "stored";

			out << std::left  << std::setw(kNameW)    << ("  " + e.virtualPath)
			    << std::right << std::setw(kSizeW)     << FormatSize(e.rawSize)
			    << std::setw(kSizeW)                   << FormatSize(e.onDiskSize)
			    << std::setw(kSavingsW)                << savingsStr.str()
			    << "  " << hashStr.str()
			    << "\n";
		}

		// Summary
		const double savings = totalRawBytes > 0
		    ? 100.0 * (1.0 - static_cast<double>(pakBytes) / static_cast<double>(totalRawBytes))
		    : 0.0;

		out << rule << "\n";
		std::ostringstream totalSavings;
		totalSavings << std::fixed << std::setprecision(1) << savings << "%";
		out << std::left  << std::setw(kNameW)    << "  TOTAL"
		    << std::right << std::setw(kSizeW)     << FormatSize(totalRawBytes)
		    << std::setw(kSizeW)                   << FormatSize(pakBytes)
		    << std::setw(kSavingsW)                << totalSavings.str()
		    << "\n";
		out << "\n";
		out << "  " << entries.size() << " entries  |  " << FormatDuration(elapsedSecs) << "\n";
	}

	// -------------------------------------------------------------------------
	// Per-file read + compress task (runs on a worker thread)
	// -------------------------------------------------------------------------

	struct FileResult
	{
		std::string            virtualPath;
		std::vector<std::byte> data;
		uint32_t               flags       = 0;
		uint64_t               rawSize     = 0;
		uint64_t               contentHash = 0;
		bool                   ok          = false;
		std::string            errorMsg;
	};

	FileResult ProcessFile(const std::string& virtualPath, const fs::path& diskPath, int compressionLevel)
	{
		FileResult result;
		result.virtualPath = virtualPath;

		std::ifstream in(diskPath, std::ios::binary | std::ios::ate);
		if (!in)
		{
			result.errorMsg = "cannot open '" + diskPath.string() + "'";
			return result;
		}

		const auto rawSize = static_cast<std::size_t>(in.tellg());
		result.rawSize     = static_cast<uint64_t>(rawSize);
		in.seekg(0);

		std::vector<std::byte> rawData(rawSize);
		in.read(reinterpret_cast<char*>(rawData.data()), static_cast<std::streamsize>(rawSize));
		if (!in)
		{
			result.errorMsg = "read error on '" + diskPath.string() + "'";
			return result;
		}

		result.contentHash = XXH3_64bits(rawData.data(), rawData.size());

		if (compressionLevel > 0 && rawSize >= kMinCompressSize && !IsAlreadyCompressed(diskPath))
		{
			const std::size_t      bound = ZSTD_compressBound(rawSize);
			std::vector<std::byte> compressed(bound);

			const std::size_t compressedSize = ZSTD_compress(
			    compressed.data(), bound,
			    rawData.data(),    rawSize,
			    compressionLevel);

			if (!ZSTD_isError(compressedSize) && compressedSize < rawSize)
			{
				compressed.resize(compressedSize);
				result.data  = std::move(compressed);
				result.flags = PAK_FLAG_ZSTD;
				result.ok    = true;
				return result;
			}
		}

		result.data = std::move(rawData);
		result.ok   = true;
		return result;
	}
} // namespace

// ---------------------------------------------------------------------------
// PakWriter
// ---------------------------------------------------------------------------

void PakWriter::AddDirectory(const fs::path& sourceDir)
{
	m_sourceDir = sourceDir;

	for (const auto& entry : fs::recursive_directory_iterator(sourceDir))
	{
		if (!entry.is_regular_file())
			continue;

		const auto rel   = entry.path().lexically_relative(sourceDir);
		const auto vpath = rel.generic_string();
		m_files.push_back({ vpath, entry.path() });
	}

	std::sort(m_files.begin(), m_files.end(),
	    [](const FileRecord& a, const FileRecord& b) { return a.virtualPath < b.virtualPath; });
}

bool PakWriter::Write(const fs::path& outPath) const
{
	const auto wallStart    = std::chrono::steady_clock::now();
	const auto manifestPath = fs::path(outPath.string() + ".manifest");
	const auto logPath      = fs::path(outPath.string() + ".log");

	// --- Incremental check ---------------------------------------------------
	const ManifestMap manifest = LoadManifest(manifestPath);
	if (IsUpToDate(outPath, manifest, m_files))
	{
		std::cout << "AssetPacker: " << outPath.filename().string() << " is up to date, skipping.\n";
		return true;
	}

	// --- Read + compress (parallel) ------------------------------------------
	if (m_compressionLevel > 0)
		std::cout << "AssetPacker: reading and compressing " << m_files.size()
		          << " file(s) (zstd level " << m_compressionLevel << ")...\n";
	else
		std::cout << "AssetPacker: reading " << m_files.size() << " file(s) (compression disabled)...\n";

	std::vector<std::future<FileResult>> futures;
	futures.reserve(m_files.size());
	for (const auto& file : m_files)
		futures.push_back(std::async(std::launch::async, ProcessFile, file.virtualPath, file.diskPath, m_compressionLevel));

	// --- Collect results and build in-memory sections ------------------------
	std::vector<char>      pathData;
	std::vector<std::byte> assetData;
	std::vector<PakEntry>  entries;
	std::vector<LogEntry>  logEntries;
	ManifestMap            newManifest;
	entries.reserve(m_files.size());
	logEntries.reserve(m_files.size());
	newManifest.reserve(m_files.size());

	uint64_t totalRawBytes = 0;
	bool     anyError      = false;
	int      fileIdx       = 0;

	for (auto& future : futures)
	{
		FileResult res = future.get();

		if (!res.ok)
		{
			std::cerr << "  ! WARNING: " << res.errorMsg << " - skipping\n";
			anyError = true;
			++fileIdx;
			continue;
		}

		totalRawBytes         += res.rawSize;
		const uint64_t onDisk  = static_cast<uint64_t>(res.data.size());

		PakEntry entry;
		entry.pathOffset  = static_cast<uint32_t>(pathData.size());
		entry.pathLen     = static_cast<uint32_t>(res.virtualPath.size());
		entry.flags       = res.flags;
		entry.dataOffset  = static_cast<uint64_t>(assetData.size());
		entry.dataSize    = onDisk;
		entry.contentHash = res.contentHash;
		entries.push_back(entry);

		logEntries.push_back({ res.virtualPath, res.rawSize, onDisk, res.contentHash, res.flags });

		pathData.insert(pathData.end(), res.virtualPath.begin(), res.virtualPath.end());
		pathData.push_back('\0');
		assetData.insert(assetData.end(), res.data.begin(), res.data.end());

		std::error_code ec;
		const auto      mtime  = fs::last_write_time(m_files[fileIdx].diskPath, ec);
		const int64_t   ticks  = ec ? 0 : mtime.time_since_epoch().count();
		newManifest[res.virtualPath] = { ticks, res.contentHash };

		// Console line
		const bool compressed = (res.flags & PAK_FLAG_ZSTD) != 0;
		std::cout << "  + " << std::left << std::setw(52) << res.virtualPath;
		if (compressed)
		{
			const double ratio = 100.0 * (1.0 - static_cast<double>(onDisk) / static_cast<double>(res.rawSize));
			std::cout << FormatSize(res.rawSize) << " -> " << FormatSize(onDisk)
			          << "  (" << std::fixed << std::setprecision(1) << ratio << "% smaller)\n";
		}
		else
		{
			std::cout << FormatSize(res.rawSize) << "  (stored raw)\n";
		}
		++fileIdx;
	}

	// --- Write pak file ------------------------------------------------------
	const uint64_t entryTableSize  = sizeof(PakEntry) * entries.size();
	const uint64_t pathDataOffset  = sizeof(PakHeader) + entryTableSize;
	const uint64_t assetDataOffset = pathDataOffset + static_cast<uint64_t>(pathData.size());

	PakHeader header;
	header.numEntries      = static_cast<uint32_t>(entries.size());
	header.pathDataOffset  = pathDataOffset;
	header.pathDataSize    = static_cast<uint64_t>(pathData.size());
	header.assetDataOffset = assetDataOffset;
	header.assetDataSize   = static_cast<uint64_t>(assetData.size());

	fs::create_directories(outPath.parent_path());

	std::cout << "AssetPacker: writing " << outPath.generic_string() << "...\n";

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

	const auto   wallEnd     = std::chrono::steady_clock::now();
	const double elapsedSecs = std::chrono::duration<double>(wallEnd - wallStart).count();

	const uint64_t pakBytes = sizeof(PakHeader)
	    + static_cast<uint64_t>(entryTableSize)
	    + static_cast<uint64_t>(pathData.size())
	    + static_cast<uint64_t>(assetData.size());

	SaveManifest(manifestPath, newManifest);
	SaveLog(logPath, m_sourceDir, outPath, m_compressionLevel, logEntries, totalRawBytes, pakBytes, elapsedSecs);

	// --- Console summary -----------------------------------------------------
	const double savings = totalRawBytes > 0
	    ? 100.0 * (1.0 - static_cast<double>(pakBytes) / static_cast<double>(totalRawBytes))
	    : 0.0;

	std::cout << "AssetPacker: done.\n";
	std::cout << "  Entries:  " << entries.size() << "\n";
	std::cout << "  Raw size: " << FormatSize(totalRawBytes) << "\n";
	std::cout << "  Pak size: " << FormatSize(pakBytes);
	if (totalRawBytes > 0)
		std::cout << "  (" << std::fixed << std::setprecision(1) << savings << "% smaller)";
	std::cout << "\n";
	std::cout << "  Time:     " << FormatDuration(elapsedSecs) << "\n";
	std::cout << "  Log:      " << logPath.generic_string() << "\n";

	return !anyError;
}
