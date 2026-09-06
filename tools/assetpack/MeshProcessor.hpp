#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

#include "PipelineUtils.hpp"

namespace aether::assetpipeline
{
	namespace MeshProcessor
	{
		struct ProcessedResult
		{
			ByteBuffer skelData;
			ByteBuffer meshData;
			ByteBuffer animsetData;
			std::vector<std::pair<std::string, ByteBuffer>> animFiles;
			std::vector<std::pair<std::string, ByteBuffer>> materialFiles;
			std::string skeletonHash;
		};

		[[nodiscard]] ProcessedResult Process(std::span<const std::byte> gltfData, const std::filesystem::path& sourcePath, const std::string& virtualPath, const std::filesystem::path& sourceDir);

		// Disk paths of every EXTERNAL buffer/image `modelPath` references (a `.gltf`'s
		// separate `.bin`/textures; a self-contained `.glb`'s embedded buffers are skipped,
		// as are `data:` URIs). For freshness checks (AssetPacker `bake-all`): editing one of
		// these must be as stale-triggering as editing the model file itself - a grafted
		// animation clip or a root-motion fix living only in an external `.bin` is otherwise
		// invisible until someone deletes the stale bake by hand. Resolution is a plain path
		// join against `modelPath`'s directory (no percent-decoding), matching this file's
		// own image-URI handling - every asset in this project uses plain ASCII filenames.
		// Best-effort: a parse failure returns an empty list rather than erroring: Process()
		// surfaces a proper error if the model is actually (re)baked.
		[[nodiscard]] std::vector<std::filesystem::path> CollectExternalSourceFiles(const std::filesystem::path& modelPath);
	} // namespace MeshProcessor
} // namespace aether::assetpipeline
