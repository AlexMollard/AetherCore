#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "assets/AssetId.hpp"

namespace aether
{
	enum class AssetType : std::uint8_t
	{
		Unknown = 0,
		Mesh,
		Model,
		Texture,
		Material,
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

	// invalid id (0) is never produced.
	[[nodiscard]] inline AssetId ComputeAssetId(const AssetSource& source) noexcept
	{
		std::uint64_t h = 1469598103934665603ull;
		const auto mix = [&h](const void* data, std::size_t n)
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (std::size_t i = 0; i < n; ++i)
			{
				h ^= bytes[i];
				h *= 1099511628211ull;
			}
		};

		const std::uint8_t typeByte = static_cast<std::uint8_t>(source.type);
		const std::uint8_t builtinByte = source.builtin ? 1u : 0u;
		mix(&typeByte, sizeof(typeByte));
		mix(&builtinByte, sizeof(builtinByte));
		mix(&source.subIndex, sizeof(source.subIndex));
		mix(source.path.data(), source.path.size());

		return AssetId{h == 0 ? 1ull : h}; // never collide with the invalid sentinel
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
} // namespace aether
