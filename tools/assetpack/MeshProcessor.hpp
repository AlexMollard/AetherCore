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
		// Output from processing a glTF/GLB file - separate asset files.
		struct ProcessedResult
		{
			ByteBuffer skelData;                                           // .skel binary (empty if no skeleton)
			ByteBuffer meshData;                                           // .mesh binary
			ByteBuffer animsetData;                                        // .animset binary (empty if no animations)
			std::vector<std::pair<std::string, ByteBuffer>> animFiles;     // (filename, data) per clip
			std::vector<std::pair<std::string, ByteBuffer>> materialFiles; // (vfsPath, binary .material data)
			std::string skeletonHash;                                      // hex string of XXH3-64 hash
		};

		// Parse a GLTF or GLB file and output separate asset files:
		// - .skel: skeleton with sorted bones + XXH3-64 hash
		// - .mesh: geometry with remapped joint indices + material path refs
		// - .animset: bundle of animation clip paths + skeleton hash
		// - .anim: individual animation clips with remapped node indices
		//
		// sourcePath is used to resolve external .bin buffer URIs referenced by
		// .gltf files; it is unused for self-contained .glb files.
		//
		// Returns empty result if the input is not valid or contains no meshes.
		[[nodiscard]] ProcessedResult Process(std::span<const std::byte> gltfData, const std::filesystem::path& sourcePath, const std::string& virtualPath, const std::filesystem::path& sourceDir);
	} // namespace MeshProcessor
} // namespace aether::assetpipeline
