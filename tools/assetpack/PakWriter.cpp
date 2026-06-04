#include "PakWriter.hpp"

#include "MaterialProcessor.hpp"
#include "MeshProcessor.hpp"
#include "PipelineUtils.hpp"
#include "SpirvProcessor.hpp"
#include "TextureProcessor.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <zstd.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
	namespace fs = std::filesystem;

	constexpr std::size_t kMinCompressSize = 64;

	bool IsAlreadyCompressed(const fs::path& path)
	{
		const auto ext = path.extension().string();
		std::string lower;
		lower.reserve(ext.size());
		for (const char c : ext)
			lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

		static constexpr std::string_view kSkip[] = {
		    ".jpg", ".jpeg", ".png",
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
	// Manifest - persists per-file mtime + hash so unchanged assets are skipped
	// -------------------------------------------------------------------------

	// Bump when the manifest format changes so that stale cached manifests
	// are automatically regenerated on the next pack.
	// This is distinct from any tool or format version — see kManifestVersion
	// if/when a separate tool-level version is needed.
	constexpr int kManifestVersion = 2;

	struct ManifestEntry
	{
		int64_t  mtimeSec;    // last_write_time as seconds since epoch
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
		// First non-empty line must be the version header.
		while (std::getline(in, line))
		{
			if (line.empty())
				continue;
			// Expected: "# AetherPak manifest vN"
			const std::string expected = "# AetherPak manifest v" + std::to_string(kManifestVersion);
			if (line != expected)
				return {}; // version mismatch - force full repack
			break;
		}

		while (std::getline(in, line))
		{
			if (line.empty() || line[0] == '#')
				continue;

			// Format: virtualPath\tmtimeSec\thashHex
			const auto t1 = line.find('\t');
			const auto t2 = line.find('\t', t1 + 1);
			if (t1 == std::string::npos || t2 == std::string::npos)
				continue;

			const std::string  vpath    = line.substr(0, t1);
			const std::string_view mstr = { line.data() + t1 + 1, t2 - t1 - 1 };
			const std::string_view hstr = { line.data() + t2 + 1, line.size() - t2 - 1 };

			ManifestEntry e{};
			std::from_chars(mstr.data(), mstr.data() + mstr.size(), e.mtimeSec);
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

		out << "# AetherPak manifest v" << kManifestVersion << "\n";
		for (const auto& [vpath, e] : map)
			out << vpath << '\t' << std::dec << e.mtimeSec << '\t' << std::hex << e.contentHash << '\n';
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

		for (const auto& file : files)
		{
			const auto it = manifest.find(file.virtualPath);
			if (it == manifest.end())
				return false;

			std::error_code ec;
			const auto mtime = fs::last_write_time(file.diskPath, ec);
			if (ec)
				return false;

			const auto mtimeSec = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
			if (mtimeSec != it->second.mtimeSec)
				return false;
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// Log - human-readable build record written to <output>.pak.log
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
	// Asset processing dispatch
	// -------------------------------------------------------------------------

	struct PakFileData
	{
		std::string virtualPath;
		std::vector<std::byte> data;
		uint32_t flags = 0;
		uint64_t rawSize = 0;
		uint64_t contentHash = 0;
	};

	struct ProcessAssetResult
	{
		std::vector<std::byte> data;
		std::string            outExt;
		std::vector<PakFileData> extraFiles;
	};

	ProcessAssetResult ProcessAsset(
	    const std::vector<std::byte>& raw,
	    const fs::path&               diskPath,
	    const std::string&            virtualPath,
	    const fs::path&               sourceDir)
	{
		const auto ext = [&] {
			std::string e = diskPath.extension().string();
			for (char& c : e)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return e;
		}();

		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
		    ext == ".tga" || ext == ".bmp" || ext == ".webp")
		{
			auto d = TextureProcessor::ToDDS(raw, diskPath, TextureProcessor::BC7Quality::Normal);
			if (!d.empty())
				return { std::move(d), ".texture", {} };
			return {};
		}

		if (ext == ".toml")
		{
			if (diskPath.filename() == "properties.toml")
			{
				auto d = MaterialProcessor::Process(raw, diskPath, sourceDir);
				if (!d.empty())
					return { std::move(d), ".material", {} };
			}
			return {};
		}

		if (ext == ".spv")
		{
			auto d = SpirvProcessor::Strip(raw);
			return { std::move(d), {}, {} };
		}

		if (ext == ".gltf" || ext == ".glb")
		{
			auto meshResult = MeshProcessor::Process(raw, diskPath, virtualPath, sourceDir);
			if (!meshResult.meshData.empty())
			{
				const std::string stem = Stem(diskPath);
				const std::string dir = fs::path(virtualPath).parent_path().generic_string();

				std::vector<PakFileData> extraFiles;

				if (!meshResult.skelData.empty())
				{
					const std::string skelPath = dir.empty() ? (stem + ".skel") : (dir + "/" + stem + ".skel");
					extraFiles.push_back({ skelPath, std::move(meshResult.skelData) });
				}
				if (!meshResult.animsetData.empty())
				{
					const std::string animsetPath = dir.empty() ? (stem + ".animset") : (dir + "/" + stem + ".animset");
					extraFiles.push_back({ animsetPath, std::move(meshResult.animsetData) });
				}
				for (auto& [fileName, animData] : meshResult.animFiles)
				{
					if (!animData.empty())
						extraFiles.push_back({ "animations/" + fileName, std::move(animData) });
				}

				return { std::move(meshResult.meshData), ".mesh", std::move(extraFiles) };
			}
			return {};
		}

		return {};
	}

	struct PakFileResult
	{
		std::string            virtualPath;
		std::vector<std::byte> data;
		uint32_t               flags       = 0;
		uint64_t               rawSize     = 0;
		uint64_t               contentHash = 0;
		bool                   ok          = false;
		std::string            errorMsg;
		std::vector<PakFileData> extraFiles;
	};

	// -------------------------------------------------------------------------
	// Per-file read + compress task (runs on a worker thread)
	// -------------------------------------------------------------------------

	PakFileResult ProcessFile(const std::string& virtualPath, const fs::path& diskPath, const fs::path& sourceDir, int compressionLevel)
	{
		PakFileResult result;
		result.virtualPath = virtualPath;

		// Reusable compression context — avoids per-call alloc/free overhead
		ZSTD_CCtx* cctx = (compressionLevel > 0) ? ZSTD_createCCtx() : nullptr;

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

		// Asset processing: transcode textures -> BCn DDS, strip SPIR-V debug info,
		// convert GLTF -> flat AEBN binary.  On success the virtual path extension
		// is replaced so the engine sees a consistent type regardless of source format.
		auto procResult = ProcessAsset(rawData, diskPath, virtualPath, sourceDir);
		if (!procResult.data.empty())
		{
			rawData = std::move(procResult.data);
			if (!procResult.outExt.empty())
			{
				const auto stem = fs::path(virtualPath).stem().string();
				const auto parent = fs::path(virtualPath).parent_path().generic_string();
				result.virtualPath = parent.empty()
				    ? stem + procResult.outExt
				    : parent + "/" + stem + procResult.outExt;
			}

			// Material files: rewrite virtual path to the .material path
			if (procResult.outExt == ".material")
			{
				auto dir = fs::path(virtualPath).parent_path();
				const std::string dirName = dir.filename().string();
				if (dirName.size() < 9 || dirName.substr(dirName.size() - 9) != ".material")
					dir += ".material";
				result.virtualPath = dir.generic_string();
			}
		}

		// Queue extra files from asset processing (e.g., .skel, .anim, .animset from mesh processing)
		for (auto& extra : procResult.extraFiles)
		{
			if (extra.data.empty()) continue;

			extra.contentHash = XXH3_64bits(extra.data.data(), extra.data.size());
			extra.rawSize = static_cast<uint64_t>(extra.data.size());

			if (compressionLevel > 0 && extra.rawSize >= kMinCompressSize && !IsAlreadyCompressed(diskPath))
			{
				const std::size_t bound = ZSTD_compressBound(extra.rawSize);
				std::vector<std::byte> compressed(bound);
				const std::size_t compressedSize = ZSTD_compressCCtx(
				    cctx,
				    compressed.data(), bound,
				    extra.data.data(), extra.rawSize,
				    compressionLevel);
				if (!ZSTD_isError(compressedSize) && compressedSize < extra.rawSize)
				{
					compressed.resize(compressedSize);
					extra.data = std::move(compressed);
					extra.flags |= PAK_FLAG_ZSTD;
				}
			}

			result.extraFiles.push_back(std::move(extra));
		}

		result.contentHash = XXH3_64bits(rawData.data(), rawData.size());

		// Use the post-processing size for compression - rawSize holds the original
		// on-disk file size which may differ greatly after asset transcoding.
		const std::size_t processedSize = rawData.size();
		result.rawSize = static_cast<uint64_t>(processedSize);

		if (compressionLevel > 0 && processedSize >= kMinCompressSize && !IsAlreadyCompressed(diskPath))
		{
			const std::size_t      bound = ZSTD_compressBound(processedSize);
			std::vector<std::byte> compressed(bound);

			const std::size_t compressedSize = ZSTD_compressCCtx(
			    cctx,
			    compressed.data(), bound,
			    rawData.data(),    processedSize,
			    compressionLevel);

			if (!ZSTD_isError(compressedSize) && compressedSize < rawSize)
			{
				compressed.resize(compressedSize);
				result.data  = std::move(compressed);
				result.flags = PAK_FLAG_ZSTD;
				result.ok    = true;
				if (cctx) ZSTD_freeCCtx(cctx);
				return result;
			}
		}

		result.data = std::move(rawData);
		result.ok   = true;
		if (cctx) ZSTD_freeCCtx(cctx);
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

	std::vector<std::future<PakFileResult>> futures;
	futures.reserve(m_files.size());
	for (const auto& file : m_files)
		futures.push_back(std::async(std::launch::async, ProcessFile, file.virtualPath, file.diskPath, m_sourceDir, m_compressionLevel));

	// --- Collect results ----------------------------------------------------
	std::vector<PakFileResult> allResults;
	allResults.reserve(futures.size());
	bool anyError = false;
	int fileIdx = 0;

	for (auto& future : futures)
	{
		PakFileResult res = future.get();
		if (!res.ok)
		{
			std::cerr << "  ! WARNING: " << res.errorMsg << " - skipping\n";
			anyError = true;
		}
		allResults.push_back(std::move(res));
		++fileIdx;
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

	for (std::size_t ri = 0; ri < allResults.size(); ++ri)
	{
		const PakFileResult& res = allResults[ri];
		if (!res.ok) continue;

		items.push_back({ res.virtualPath, res.data, res.flags, res.rawSize, res.contentHash });

		for (const auto& extra : res.extraFiles)
		{
			if (extra.data.empty()) continue;
			items.push_back({ extra.virtualPath, extra.data, extra.flags, extra.rawSize, extra.contentHash });
		}
	}

	std::sort(items.begin(), items.end(),
		[](const PackItem& a, const PackItem& b) { return a.virtualPath < b.virtualPath; });

	// --- Build in-memory sections -------------------------------------------
	std::vector<char>      pathData;
	std::vector<std::byte> assetData;
	std::vector<PakEntry>  entries;
	std::vector<LogEntry>  logEntries;
	ManifestMap            newManifest;
	entries.reserve(items.size());
	logEntries.reserve(items.size());
	newManifest.reserve(m_files.size());

	uint64_t totalRawBytes = 0;

	for (const auto& item : items)
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

		logEntries.push_back({ item.virtualPath, item.rawSize, onDisk, item.contentHash, item.flags });

		pathData.insert(pathData.end(), item.virtualPath.begin(), item.virtualPath.end());
		pathData.push_back('\0');
		assetData.insert(assetData.end(), item.data.begin(), item.data.end());

		// Console line
		const bool compressed = (item.flags & PAK_FLAG_ZSTD) != 0;
		std::cout << "  + " << std::left << std::setw(52) << item.virtualPath;
		if (compressed)
		{
			const double ratio = 100.0 * (1.0 - static_cast<double>(onDisk) / static_cast<double>(item.rawSize));
			std::cout << FormatSize(item.rawSize) << " -> " << FormatSize(onDisk)
			          << "  (" << std::fixed << std::setprecision(1) << ratio << "% smaller)\n";
		}
		else
		{
			std::cout << FormatSize(item.rawSize) << "  (stored raw)\n";
		}
	}

	// Build manifest from source files (mtime-based) + derived files (hash-only)
	newManifest.reserve(m_files.size() + items.size());
	for (std::size_t ri = 0; ri < allResults.size(); ++ri)
	{
		const PakFileResult& res = allResults[ri];
		if (!res.ok) continue;

		std::error_code ec;
		const auto mtime = fs::last_write_time(m_files[ri].diskPath, ec);
		const int64_t mtimeSec = ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
		newManifest[m_files[ri].virtualPath] = { mtimeSec, res.contentHash };

		for (const auto& extra : res.extraFiles)
		{
			if (extra.data.empty()) continue;
			newManifest[extra.virtualPath] = { 0, extra.contentHash };
		}
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

	const auto   wallEnd     = std::chrono::steady_clock::now();
	const double elapsedSecs = std::chrono::duration<double>(wallEnd - wallStart).count();

	const uint64_t pakBytes = sizeof(PakHeader)
	    + static_cast<uint64_t>(entryTableSize)
	    + static_cast<uint64_t>(pathData.size())
	    + static_cast<uint64_t>(assetData.size());

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
		out.write(pathData.data(), static_cast<std::streamsize>(pathData.size()));
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
