#pragma once
#include <cstddef>
#include <filesystem>
#include <vector>

namespace MeshProcessor
{
	// Parse a GLTF or GLB file and write all mesh primitives to the flat AEBN
	// binary format defined in include/AeBnFormat.hpp.
	//
	// sourcePath is used to resolve external .bin buffer URIs referenced by
	// .gltf files; it is unused for self-contained .glb files.
	//
	// Returns empty if the input is not a valid GLTF/GLB or contains no meshes.
	// Output extension for the virtual path is always ".mesh".
	std::vector<std::byte> ToBinary(
	    const std::vector<std::byte>& gltfData,
	    const std::filesystem::path&  sourcePath);
} // namespace MeshProcessor
