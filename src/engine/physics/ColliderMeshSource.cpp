#include "physics/ColliderMeshSource.hpp"

#include "assets/GltfAsset.hpp"

namespace aether
{
	std::optional<ColliderMeshGeometry> BuildColliderMeshGeometry(const assets::GltfAsset& asset)
	{
		std::size_t totalVertices = 0;
		for (const assets::GltfPrimitive& prim: asset.primitives)
		{
			totalVertices += prim.vertices.size();
		}
		if (totalVertices == 0 || totalVertices > kMaxColliderMeshVertices)
		{
			return std::nullopt;
		}

		ColliderMeshGeometry geo;
		geo.positions.reserve(totalVertices);
		for (const assets::GltfPrimitive& prim: asset.primitives)
		{
			const auto baseIndex = static_cast<std::uint32_t>(geo.positions.size());
			for (const Mesh::Vertex& v: prim.vertices)
			{
				geo.positions.push_back(v.position);
			}
			// GltfAsset's own loader already validated every index against this
			// primitive's vertex count (GltfAsset.cpp's LoadFromMesh) - re-validate here
			// anyway rather than trust that invariant across a module boundary; a
			// triangle naming an out-of-range LOCAL index is skipped, not used to read
			// past geo.positions.
			for (std::size_t i = 0; i + 2 < prim.indices.size(); i += 3)
			{
				if (prim.indices[i] >= prim.vertices.size() || prim.indices[i + 1] >= prim.vertices.size() || prim.indices[i + 2] >= prim.vertices.size())
				{
					continue;
				}
				geo.indices.push_back(baseIndex + prim.indices[i]);
				geo.indices.push_back(baseIndex + prim.indices[i + 1]);
				geo.indices.push_back(baseIndex + prim.indices[i + 2]);
			}
		}
		return geo;
	}
} // namespace aether
