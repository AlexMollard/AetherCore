#pragma once

#include <cstdint>
#include <vector>

#include "mesh/Mesh.hpp"

namespace aether::MeshGen
{
	struct MeshData
	{
		std::vector<Mesh::Vertex> vertices;
		std::vector<std::uint32_t> indices;
	};

	struct PlaneDesc
	{
		int segmentsX = 10;
		int segmentsY = 10;
		float uvScale = 1.0f;
	};

	[[nodiscard]] MeshData GeneratePlane(const PlaneDesc& desc);

	struct UVSphereDesc
	{
		int stacks = 16;
		int slices = 32;
		float uvScale = 1.0f;
	};

	[[nodiscard]] MeshData GenerateUVSphere(const UVSphereDesc& desc);

	struct CylinderDesc
	{
		int segments = 32;
		bool caps = true;
		float uvScaleRadial = 1.0f;
		float uvScaleAxial = 1.0f;
	};

	[[nodiscard]] MeshData GenerateCylinder(const CylinderDesc& desc);

} // namespace aether::MeshGen
