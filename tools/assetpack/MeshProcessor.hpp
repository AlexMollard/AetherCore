#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

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

		// One submesh for ComputeFoliageSway: its index range, and whether its material is
		// scenery foliage (foliage + baked_lighting, not sky - Foliage.slangh's IsSceneryFoliage).
		struct SwaySubMesh
		{
			std::uint32_t firstIndex = 0;
			std::uint32_t indexCount = 0;
			bool foliage = false;
		};

		// Wind sway weight per vertex (0 = still, 1 = full sway), baked into uv2.x of the scenery
		// foliage vertices and read by Foliage.slangh. `positions` is xyz per vertex. A vertex
		// touching geometry other than its own card (the terrain a grass fringe is welded to, the
		// ground under a tuft, the next card) is an anchor and never moves, and the weight grows
		// with distance from the card's anchors, so only free tips sway and no seam opens. A card
		// lying on other geometry for most of its vertices (fringe skirts, decals) stays still.
		// Vertices outside the foliage submeshes get 0.
		[[nodiscard]] std::vector<float> ComputeFoliageSway(std::span<const float> positions, std::span<const std::uint32_t> indices, std::span<const SwaySubMesh> subMeshes);
	} // namespace MeshProcessor
} // namespace aether::assetpipeline
