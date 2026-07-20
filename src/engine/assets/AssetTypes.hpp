#pragma once

#include <cstddef>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "assets/AssetId.hpp"
#include "utils/Hash.hpp"

namespace aether
{
	enum class AssetType : std::uint8_t
	{
		Unknown = 0,
		Mesh,
		Model,
		Texture,
		Material,
		SpriteAtlas,
		SpriteAnimation,
		TileSet,
		TileMap,
		PhysicsMaterial2D,
	};

	[[nodiscard]] inline const char* AssetTypeName(AssetType type) noexcept
	{
		switch (type)
		{
			case AssetType::Mesh:
				return "Mesh";
			case AssetType::Model:
				return "Model";
			case AssetType::Texture:
				return "Texture";
			case AssetType::Material:
				return "Material";
			case AssetType::SpriteAtlas:
				return "SpriteAtlas";
			case AssetType::SpriteAnimation:
				return "SpriteAnimation";
			case AssetType::TileSet:
				return "TileSet";
			case AssetType::TileMap:
				return "TileMap";
			case AssetType::PhysicsMaterial2D:
				return "PhysicsMaterial2D";
			case AssetType::Unknown:
			default:
				return "Unknown";
		}
	}

	struct AssetSource
	{
		AssetType type = AssetType::Unknown;
		std::string path;
		std::int32_t subIndex = -1;
		bool builtin = false;

		[[nodiscard]] bool operator==(const AssetSource& o) const
		{
			return type == o.type && subIndex == o.subIndex && builtin == o.builtin && path == o.path;
		}
	};

	// Stable identity for an authored object inside an asset (for example, a
	// sprite region inside an atlas). The persistent key is generated once and
	// stored by the asset format; names and array positions are deliberately not
	// part of the contract, so reordering or renaming cannot redirect references.
	struct AssetObjectId
	{
		std::uint64_t value = 0;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return value != 0;
		}

		auto operator<=>(const AssetObjectId&) const = default;
	};

	// invalid id (0) is never produced.
	[[nodiscard]] inline AssetId ComputeAssetId(const AssetSource& source) noexcept
	{
		utils::Fnv1aHasher hasher;
		const std::uint8_t typeByte = static_cast<std::uint8_t>(source.type);
		const std::uint8_t builtinByte = source.builtin ? 1u : 0u;
		hasher.MixValue(typeByte);
		hasher.MixValue(builtinByte);
		hasher.MixValue(source.subIndex);
		hasher.Mix(source.path);

		const std::uint64_t h = hasher.Value();
		return AssetId{h == 0 ? 1ull : h}; // never collide with the invalid sentinel
	}

	[[nodiscard]] inline AssetObjectId ComputeAssetObjectId(AssetId owner, std::string_view persistentKey) noexcept
	{
		utils::Fnv1aHasher hasher;
		hasher.MixValue(owner.value);
		hasher.Mix(persistentKey);
		const std::uint64_t h = hasher.Value();
		return AssetObjectId{h == 0 ? 1ull : h};
	}

	[[nodiscard]] inline AssetSource MakePrimitiveMeshSource(std::string kindName)
	{
		return AssetSource{.type = AssetType::Mesh, .path = std::move(kindName), .subIndex = -1, .builtin = true};
	}

	[[nodiscard]] inline AssetSource MakeModelMeshSource(std::string vfsPath, int primitiveIndex)
	{
		return AssetSource{.type = AssetType::Mesh, .path = std::move(vfsPath), .subIndex = primitiveIndex, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakeModelSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::Model, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakeTextureSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::Texture, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakeMaterialPresetSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::Material, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakeSpriteAtlasSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::SpriteAtlas, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakeSpriteAnimationSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::SpriteAnimation, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakeTileSetSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::TileSet, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakeTileMapSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::TileMap, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}

	[[nodiscard]] inline AssetSource MakePhysicsMaterial2DSource(std::string vfsPath)
	{
		return AssetSource{.type = AssetType::PhysicsMaterial2D, .path = std::move(vfsPath), .subIndex = -1, .builtin = false};
	}
} // namespace aether
