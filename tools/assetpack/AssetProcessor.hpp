#pragma once

#include "MaterialProcessor.hpp"
#include "MeshProcessor.hpp"
#include "PipelineUtils.hpp"
#include "SpirvProcessor.hpp"
#include "TextureProcessor.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>
#include <zstd.h>

namespace fs = std::filesystem;

inline constexpr std::size_t kMinCompressSize = 64;

inline bool IsAlreadyCompressed(const fs::path& path)
{
	const auto ext = path.extension().string();
	std::string lower;
	lower.reserve(ext.size());
	for (const char c: ext)
	{
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	static constexpr std::string_view kSkip[] = {
	        ".jpg",
	        ".jpeg",
	        ".png",
	        ".dds",
	        ".ktx",
	        ".ktx2",
	        ".basis",
	        ".ogg",
	        ".mp3",
	        ".opus",
	        ".flac",
	        ".aac",
	        ".zip",
	        ".gz",
	        ".br",
	        ".zst",
	};
	for (const auto& s: kSkip)
	{
		if (lower == s)
		{
			return true;
		}
	}
	return false;
}

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
	std::string outExt;
	std::vector<PakFileData> extraFiles;
	bool skipSource = false; // true = don't pack the source file (extras still emitted)
};

inline ProcessAssetResult ProcessAsset(const std::vector<std::byte>& raw, const fs::path& diskPath, const std::string& virtualPath, const fs::path& sourceDir)
{
	const auto ext = [&]
	{
		std::string e = diskPath.extension().string();
		for (char& c: e)
		{
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return e;
	}();

	if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".webp")
	{
		auto d = TextureProcessor::ToDDS(raw, diskPath, TextureProcessor::BC7Quality::Normal);
		if (!d.empty())
		{
			return {std::move(d), ".texture", {}};
		}
		return {};
	}

	if (ext == ".toml")
	{
		if (diskPath.filename() == "properties.toml")
		{
			auto d = MaterialProcessor::Process(raw, diskPath, sourceDir);
			if (!d.empty())
			{
				return {std::move(d), ".material", {}};
			}
		}
		return {};
	}

	if (ext == ".spv")
	{
		auto d = SpirvProcessor::Strip(raw);
		return {std::move(d), {}, {}};
	}

	if (ext == ".gltf" || ext == ".glb")
	{
		auto meshResult = MeshProcessor::Process(raw, diskPath, virtualPath, sourceDir);

		// Emit extra files (anim, skel, material) even when meshData is empty (animation-only glTFs).
		const std::string stem = Stem(diskPath);
		const std::string dir = fs::path(virtualPath).parent_path().generic_string();

		std::vector<PakFileData> extraFiles;

		if (!meshResult.skelData.empty())
		{
			const std::string skelPath = dir.empty() ? (stem + ".skel") : (dir + "/" + stem + ".skel");
			extraFiles.push_back({skelPath, std::move(meshResult.skelData)});
		}
		if (!meshResult.animsetData.empty())
		{
			const std::string animsetPath = dir.empty() ? (stem + ".animset") : (dir + "/" + stem + ".animset");
			extraFiles.push_back({animsetPath, std::move(meshResult.animsetData)});
		}
		for (auto& [fileName, animData]: meshResult.animFiles)
		{
			if (!animData.empty())
			{
				extraFiles.push_back({"animations/" + fileName, std::move(animData)});
			}
		}
		for (auto& [matVfsPath, matData]: meshResult.materialFiles)
		{
			if (!matData.empty())
			{
				extraFiles.push_back({matVfsPath, std::move(matData)});
			}
		}

		if (!meshResult.meshData.empty())
		{
			return {std::move(meshResult.meshData), ".mesh", std::move(extraFiles)};
		}

		// Animation-only (no mesh): emit extras, skip the source .gltf in the PAK.
		return {{}, {}, std::move(extraFiles), /*skipSource=*/true};
	}

	// .bin files are only used as glTF buffer data, already consumed by cgltf during mesh processing.
	if (ext == ".bin")
	{
		return {{}, {}, {}, /*skipSource=*/true};
	}

	return {};
}

struct PakFileResult
{
	std::string virtualPath;
	std::vector<std::byte> data;
	uint32_t flags = 0;
	uint64_t rawSize = 0;
	uint64_t contentHash = 0;
	bool ok = false;
	bool hasPrimary = true; // false = skip the primary file entry (extras still emitted)
	std::string errorMsg;
	std::vector<PakFileData> extraFiles;
};

inline PakFileResult ProcessFile(const std::string& virtualPath, const fs::path& diskPath, const fs::path& sourceDir, int compressionLevel)
{
	PakFileResult result;
	result.virtualPath = virtualPath;

	ZSTD_CCtx* cctx = (compressionLevel > 0) ? ZSTD_createCCtx() : nullptr;

	std::ifstream in(diskPath, std::ios::binary | std::ios::ate);
	if (!in)
	{
		result.errorMsg = "cannot open '" + diskPath.string() + "'";
		return result;
	}

	const auto rawSize = static_cast<std::size_t>(in.tellg());
	result.rawSize = static_cast<uint64_t>(rawSize);
	in.seekg(0);

	std::vector<std::byte> rawData(rawSize);
	in.read(reinterpret_cast<char*>(rawData.data()), static_cast<std::streamsize>(rawSize));
	if (!in)
	{
		result.errorMsg = "read error on '" + diskPath.string() + "'";
		return result;
	}

	auto procResult = ProcessAsset(rawData, diskPath, virtualPath, sourceDir);

	if (procResult.skipSource)
	{
		// Don't pack the source file - only emit extra files (anims, materials, etc.).
		result.hasPrimary = false;
		result.ok = true;

		for (auto& extra: procResult.extraFiles)
		{
			if (extra.data.empty())
			{
				continue;
			}
			extra.contentHash = XXH3_64bits(extra.data.data(), extra.data.size());
			extra.rawSize = static_cast<uint64_t>(extra.data.size());
			if (compressionLevel > 0 && extra.rawSize >= kMinCompressSize)
			{
				const std::size_t bound = ZSTD_compressBound(extra.rawSize);
				std::vector<std::byte> compressed(bound);
				const std::size_t compressedSize = ZSTD_compressCCtx(cctx, compressed.data(), bound, extra.data.data(), extra.rawSize, compressionLevel);
				if (!ZSTD_isError(compressedSize) && compressedSize < extra.rawSize)
				{
					compressed.resize(compressedSize);
					extra.data = std::move(compressed);
					extra.flags |= PAK_FLAG_ZSTD;
				}
			}
			result.extraFiles.push_back(std::move(extra));
		}

		result.rawSize = 0;
		result.contentHash = 0;
		if (cctx)
		{
			ZSTD_freeCCtx(cctx);
		}
		return result;
	}

	if (!procResult.data.empty())
	{
		rawData = std::move(procResult.data);
		if (!procResult.outExt.empty())
		{
			const auto stem = fs::path(virtualPath).stem().string();
			const auto parent = fs::path(virtualPath).parent_path().generic_string();
			result.virtualPath = parent.empty() ? stem + procResult.outExt : parent + "/" + stem + procResult.outExt;
		}

		if (procResult.outExt == ".material")
		{
			auto dir = fs::path(virtualPath).parent_path();
			const std::string dirName = dir.filename().string();
			if (dirName.size() < 9 || dirName.substr(dirName.size() - 9) != ".material")
			{
				dir += ".material";
			}
			result.virtualPath = dir.generic_string();
		}
	}

	for (auto& extra: procResult.extraFiles)
	{
		if (extra.data.empty())
		{
			continue;
		}

		extra.contentHash = XXH3_64bits(extra.data.data(), extra.data.size());
		extra.rawSize = static_cast<uint64_t>(extra.data.size());

		if (compressionLevel > 0 && extra.rawSize >= kMinCompressSize && !IsAlreadyCompressed(diskPath))
		{
			const std::size_t bound = ZSTD_compressBound(extra.rawSize);
			std::vector<std::byte> compressed(bound);
			const std::size_t compressedSize = ZSTD_compressCCtx(cctx, compressed.data(), bound, extra.data.data(), extra.rawSize, compressionLevel);
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

	const std::size_t processedSize = rawData.size();
	result.rawSize = static_cast<uint64_t>(processedSize);

	if (compressionLevel > 0 && processedSize >= kMinCompressSize && !IsAlreadyCompressed(diskPath))
	{
		const std::size_t bound = ZSTD_compressBound(processedSize);
		std::vector<std::byte> compressed(bound);

		const std::size_t compressedSize = ZSTD_compressCCtx(cctx, compressed.data(), bound, rawData.data(), processedSize, compressionLevel);

		if (!ZSTD_isError(compressedSize) && compressedSize < rawSize)
		{
			compressed.resize(compressedSize);
			result.data = std::move(compressed);
			result.flags = PAK_FLAG_ZSTD;
			result.ok = true;
			if (cctx)
			{
				ZSTD_freeCCtx(cctx);
			}
			return result;
		}
	}

	result.data = std::move(rawData);
	result.ok = true;
	if (cctx)
	{
		ZSTD_freeCCtx(cctx);
	}
	return result;
}
