#include "PakWriter.hpp"

#include "AssetProcessor.hpp"
#include "PakLog.hpp"
#include "PakManifest.hpp"
#include "ThreadPool.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <PakFormat.hpp>

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	namespace
	{
		bool IsExcludedProjectDirectory(const fs::path& rel)
		{
			const auto it = rel.begin();
			if (it == rel.end())
			{
				return false;
			}
			const std::string first = it->generic_string();
			// ".aether" is the editor's own corner of a project: the launcher thumbnail and
			// crash-recovery autosaves. Packing it shipped several megabytes of dead weight
			// AND put snapshots of the developer's in-progress scenes in the players' build.
			return first == "Builds" || first == "artifacts" || first == "scripts" || first == ".git" || first == ".vs" || first == ".aether";
		}

		bool IsExcludedProjectFile(const fs::path& rel)
		{
			auto it = rel.begin();
			if (it == rel.end())
			{
				return false;
			}
			const std::string first = it->generic_string();
			++it;
			if (it == rel.end())
			{
				return false;
			}
			const std::string second = it->generic_string();
			return first == ".project" && second == "publish.toml";
		}

		std::string ApplyPrefix(std::string_view prefix, const fs::path& rel)
		{
			std::string relStr = rel.generic_string();
			std::string p(prefix);
			while (!p.empty() && p.back() == '/')
			{
				p.pop_back();
			}
			if (p.empty())
			{
				return relStr;
			}
			return p + "/" + relStr;
		}
	} // namespace

	void PakWriter::AddDirectory(const fs::path& sourceDir)
	{
		m_sourceDir = sourceDir;

		for (fs::recursive_directory_iterator it(sourceDir), end; it != end; ++it)
		{
			const auto& entry = *it;
			const auto rel = entry.path().lexically_relative(sourceDir);
			if (entry.is_directory() && IsExcludedProjectDirectory(rel))
			{
				it.disable_recursion_pending();
				continue;
			}
			if (entry.is_regular_file() && (IsExcludedProjectFile(rel) || rel.generic_string() == "ProjectSettings.toml" || rel.extension() == ".slang"))
			{
				continue;
			}
			if (!entry.is_regular_file())
			{
				continue;
			}

			const auto vpath = rel.generic_string();
			m_files.push_back({vpath, entry.path(), sourceDir});
		}

		std::sort(m_files.begin(), m_files.end(), [](const FileRecord& a, const FileRecord& b) { return a.virtualPath < b.virtualPath; });
	}

	void PakWriter::AddDirectoryAs(const fs::path& sourceDir, std::string_view vpathPrefix)
	{
		for (fs::recursive_directory_iterator it(sourceDir), end; it != end; ++it)
		{
			const auto& entry = *it;
			if (!entry.is_regular_file())
			{
				continue;
			}

			// diagnostic artifact that must not leak into the shipped pak.
			if (entry.path().extension() != ".spv")
			{
				continue;
			}

			const auto rel = entry.path().lexically_relative(sourceDir);
			m_files.push_back({ApplyPrefix(vpathPrefix, rel), entry.path(), sourceDir});
		}

		std::sort(m_files.begin(), m_files.end(), [](const FileRecord& a, const FileRecord& b) { return a.virtualPath < b.virtualPath; });
	}

	bool PakWriter::Write(const fs::path& outPath) const
	{
		const auto wallStart = std::chrono::steady_clock::now();
		const auto manifestPath = fs::path(outPath.string() + ".manifest");
		const auto logPath = fs::path(outPath.string() + ".log");

		const ManifestMap manifest = LoadManifest(manifestPath);
		if (IsUpToDate(outPath, manifest, m_files))
		{
			std::cout << "AssetPacker: " << outPath.filename().string() << " is up to date, skipping.\n";
			return true;
		}

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
			futures.push_back(pool.submit([virtualPath = file.virtualPath, diskPath = file.diskPath, sourceDir = file.sourceDir, compressionLevel = m_compressionLevel]() { return ProcessFile(virtualPath, diskPath, sourceDir, compressionLevel); }));
		}

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

		{
			std::ostringstream manifest;
			manifest << "format=AetherPakManifest\n";
			manifest << "pakVersion=" << PAK_VERSION << "\n";
			manifest << "pipelineVersion=" << PAK_PIPELINE_VERSION << "\n";
			manifest << "sourceFileCount=" << m_files.size() << "\n";
			manifest << "entryCount=" << items.size() << "\n";

			const std::string manifestText = manifest.str();
			std::vector<std::byte> manifestData(manifestText.size());
			std::memcpy(manifestData.data(), manifestText.data(), manifestText.size());
			const uint64_t manifestHash = XXH3_64bits(manifestData.data(), manifestData.size());
			items.push_back({PAK_MANIFEST_PATH, std::move(manifestData), 0, static_cast<uint64_t>(manifestText.size()), manifestHash});
		}

		std::sort(items.begin(), items.end(), [](const PackItem& a, const PackItem& b) { return a.virtualPath < b.virtualPath; });

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
			const auto* const p = reinterpret_cast<const std::byte*>(s.data());
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

		const uint64_t entryTableSize = sizeof(PakEntry) * entries.size();
		const uint64_t pathDataOffset = sizeof(PakHeader) + entryTableSize;
		const uint64_t assetDataOffset = pathDataOffset + static_cast<uint64_t>(pathData.size());

		XXH3_state_t* xstate = XXH3_createState();
		XXH3_64bits_reset(xstate);
		XXH3_64bits_update(xstate, entries.data(), static_cast<size_t>(entryTableSize));
		XXH3_64bits_update(xstate, pathData.data(), pathData.size());
		const uint64_t indexHash = XXH3_64bits_digest(xstate);
		XXH3_freeState(xstate);

		PakHeader header;
		header.numEntries = static_cast<uint32_t>(entries.size());
		header.pathDataOffset = pathDataOffset;
		header.pathDataSize = static_cast<uint64_t>(pathData.size());
		header.assetDataOffset = assetDataOffset;
		header.assetDataSize = static_cast<uint64_t>(assetData.size());
		header.indexHash = indexHash;

		fs::create_directories(outPath.parent_path());

		const auto wallEnd = std::chrono::steady_clock::now();
		const double elapsedSecs = std::chrono::duration<double>(wallEnd - wallStart).count();

		const uint64_t pakBytes = sizeof(PakHeader) + static_cast<uint64_t>(entryTableSize) + static_cast<uint64_t>(pathData.size()) + static_cast<uint64_t>(assetData.size());

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
} // namespace aether::assetpipeline
