#pragma once

#include <optional>

#include "mesh/MeshGen.hpp"
#include "mesh/PrimitiveMesh.hpp"

namespace aether
{
	// CPU geometry shared by native uploads and browser rendering.
	[[nodiscard]] std::optional<MeshGen::MeshData> GeneratePrimitiveMesh(PrimitiveMesh primitive);
} // namespace aether
