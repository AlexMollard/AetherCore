#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ostream>
#include <vector>

#include <doctest/doctest.h>

#include <BinaryFormats.hpp>

#include "assets/AssetTypes.hpp"
#include "assets/GltfAsset.hpp"

using namespace aether;

TEST_CASE("2D asset sources have stable type-separated identities")
{
	const AssetSource atlas = MakeSpriteAtlasSource("project://sprites/hero.atlas.toml");
	const AssetSource animation = MakeSpriteAnimationSource("project://sprites/hero.atlas.toml");

	CHECK(ComputeAssetId(atlas) == ComputeAssetId(atlas));
	CHECK(ComputeAssetId(atlas) != ComputeAssetId(animation));
	CHECK(std::string_view{AssetTypeName(atlas.type)} == "SpriteAtlas");
}

TEST_CASE("asset object identity survives authored rename and reorder")
{
	const AssetId atlas = ComputeAssetId(MakeSpriteAtlasSource("project://sprites/hero.atlas.toml"));
	const AssetObjectId idle = ComputeAssetObjectId(atlas, "019f65a8-72ad-7000-8000-000000000001");
	const AssetObjectId run = ComputeAssetObjectId(atlas, "019f65a8-72ad-7000-8000-000000000002");

	CHECK(idle.IsValid());
	CHECK(idle == ComputeAssetObjectId(atlas, "019f65a8-72ad-7000-8000-000000000001"));
	CHECK(idle != run);
}


namespace
{
	// Minimal .mesh blob in the exact layout the packer writes (header, vertex blob,
	// uint32 index blob, submesh headers) so corrupted-header cases can be fed to
	// GltfAsset::LoadFromMemory directly.
	std::vector<std::byte> MakeMeshBytes(std::uint32_t vertexCount, const std::vector<std::uint32_t>& indices, const std::vector<SubMeshHeaderDisk>& subMeshes)
	{
		MeshHeaderDisk hdr{};
		std::memcpy(hdr.magic, MESH_MAGIC, 4);
		hdr.version = MESH_VERSION;
		hdr.vertexCount = vertexCount;
		hdr.indexCount = static_cast<std::uint32_t>(indices.size());
		hdr.subMeshCount = static_cast<std::uint32_t>(subMeshes.size());
		hdr.indexType = 1; // uint32 indices

		std::vector<std::byte> bytes;
		const auto append = [&bytes](const void* data, std::size_t size)
		{
			const auto* first = static_cast<const std::byte*>(data);
			bytes.insert(bytes.end(), first, first + size);
		};
		append(&hdr, sizeof(hdr));

		const DiskMeshVertex vert{};
		for (std::uint32_t v = 0; v < vertexCount; ++v)
		{
			append(&vert, sizeof(vert));
		}
		for (const std::uint32_t idx: indices)
		{
			append(&idx, sizeof(idx));
		}
		for (const SubMeshHeaderDisk& sm: subMeshes)
		{
			append(&sm, sizeof(sm));
		}
		return bytes;
	}
} // namespace

TEST_CASE("a well-formed packed mesh loads from memory")
{
	SubMeshHeaderDisk sm{};
	sm.firstIndex = 0;
	sm.indexCount = 6;

	const auto asset = aether::assets::GltfAsset::LoadFromMemory(MakeMeshBytes(3, {0, 1, 2, 0, 2, 1}, {sm}), "quad.mesh");
	REQUIRE(asset.has_value());
	REQUIRE(asset->primitives.size() == 1);
	REQUIRE(asset->primitives[0].indices.size() == 6);
	REQUIRE(asset->primitives[0].vertices.size() == 3);
}

TEST_CASE("a submesh index range past the index blob is rejected")
{
	SubMeshHeaderDisk sm{};
	sm.firstIndex = 5; // 5 + 3 > 3 indices in the blob
	sm.indexCount = 3;

	const auto asset = aether::assets::GltfAsset::LoadFromMemory(MakeMeshBytes(3, {0, 1, 2}, {sm}), "bad-range.mesh");
	CHECK_FALSE(asset.has_value());
}

TEST_CASE("an index referencing a missing vertex is rejected")
{
	SubMeshHeaderDisk sm{};
	sm.firstIndex = 0;
	sm.indexCount = 3;

	CHECK_FALSE(aether::assets::GltfAsset::LoadFromMemory(MakeMeshBytes(2, {0, 1, 7}, {sm}), "bad-index.mesh").has_value());
	// Same corruption on the submesh-less path, where indices are uploaded verbatim.
	CHECK_FALSE(aether::assets::GltfAsset::LoadFromMemory(MakeMeshBytes(2, {0, 1, 7}, {}), "bad-index-raw.mesh").has_value());
}