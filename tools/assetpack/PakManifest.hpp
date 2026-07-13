#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <PakFormat.hpp>

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	// Bump when the manifest format changes so that stale cached manifests
	// are automatically regenerated on the next pack.
	inline constexpr int kManifestVersion = 5;

	// Incremental-cache header line. Encodes the manifest schema version AND the
	// on-disk pak format + pipeline versions, so bumping any of them invalidates a
	// stale cached manifest and forces a full repack (otherwise an "up to date"
	// pak could be left in an older on-disk format the runtime now rejects).
	inline std::string ManifestHeaderLine()
	{
		return "# AetherPak manifest v" + std::to_string(kManifestVersion) + " pak" + std::to_string(PAK_VERSION) + " pipeline" + std::to_string(PAK_PIPELINE_VERSION);
	}

	struct ManifestEntry
	{
		int64_t mtimeSec;
		uint64_t contentHash;
	};

	using ManifestMap = std::unordered_map<std::string, ManifestEntry>;

	inline ManifestMap LoadManifest(const fs::path& manifestPath)
	{
		ManifestMap map;
		std::ifstream in(manifestPath);
		if (!in)
		{
			return map;
		}

		std::string line;
		while (std::getline(in, line))
		{
			if (line.empty())
			{
				continue;
			}
			if (line != ManifestHeaderLine())
			{
				return {};
			}
			break;
		}

		while (std::getline(in, line))
		{
			if (line.empty() || line[0] == '#')
			{
				continue;
			}

			const auto t1 = line.find('\t');
			const auto t2 = line.find('\t', t1 + 1);
			if (t1 == std::string::npos || t2 == std::string::npos)
			{
				continue;
			}

			const std::string vpath = line.substr(0, t1);
			const std::string_view mstr = {line.data() + t1 + 1, t2 - t1 - 1};
			const std::string_view hstr = {line.data() + t2 + 1, line.size() - t2 - 1};

			ManifestEntry e{};
			std::from_chars(mstr.data(), mstr.data() + mstr.size(), e.mtimeSec);
			std::from_chars(hstr.data(), hstr.data() + hstr.size(), e.contentHash, 16);
			map.emplace(vpath, e);
		}
		return map;
	}

	inline void SaveManifest(const fs::path& manifestPath, const ManifestMap& map)
	{
		std::ofstream out(manifestPath);
		if (!out)
		{
			return;
		}

		out << ManifestHeaderLine() << "\n";
		for (const auto& [vpath, e]: map)
		{
			out << vpath << '\t' << std::dec << e.mtimeSec << '\t' << std::hex << e.contentHash << '\n';
		}
	}

	template<typename FileRecord>
	inline bool IsUpToDate(const fs::path& pakPath, const ManifestMap& manifest, const std::vector<FileRecord>& files)
	{
		if (!fs::exists(pakPath))
		{
			return false;
		}

		std::unordered_set<std::string> currentSources;
		currentSources.reserve(files.size());
		for (const auto& file: files)
		{
			currentSources.insert(file.virtualPath);
			const auto it = manifest.find(file.virtualPath);
			if (it == manifest.end())
			{
				return false;
			}

			std::error_code ec;
			const auto mtime = fs::last_write_time(file.diskPath, ec);
			if (ec)
			{
				return false;
			}

			const auto mtimeSec = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
			if (mtimeSec != it->second.mtimeSec)
			{
				return false;
			}
		}

		for (const auto& [virtualPath, entry]: manifest)
		{
			if (entry.mtimeSec != 0 && !currentSources.contains(virtualPath))
			{
				return false;
			}
		}
		return true;
	}
} // namespace aether::assetpipeline
