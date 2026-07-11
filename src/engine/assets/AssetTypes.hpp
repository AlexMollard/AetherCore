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
		Mesh,     // one drawable mesh: a built-in primitive OR one glTF model primitive
		Model,    // a whole glTF model (a multi-primitive container)
		Texture,  // a 2D texture
		Material, // a material preset
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

	// Canonical description of where an asset comes from. Hashing this yields a
	// stable AssetId, so an identical source always maps to the same id. Plain data
	// so it serializes trivially (used by the scene asset manifest).
	struct AssetSource
	{
		AssetType type = AssetType::Unknown;
		// A built-in primitive kind name ("cube") when `builtin`, otherwise a VFS
		// path ("project://.../foo.glb", "shaders://...").
		std::string path;
		// Sub-resource index (a glTF model primitive index); -1 for whole-file assets.
		std::int32_t subIndex = -1;
		bool builtin = false; // path is a built-in primitive kind name, not a VFS path

		[[nodiscard]] bool operator==(const AssetSource& o) const
		{
			return type == o.type && subIndex == o.subIndex && builtin == o.builtin && path == o.path;
		}
	};

	// Deterministic content hash of a source descriptor (FNV-1a 64-bit, the same
	// idiom the material/texture registries use for content-addressed dedup). The
	// invalid id (0) is never produced.
	[[nodiscard]] inline AssetId ComputeAssetId(const AssetSource& source) noexcept
	{
		std::uint64_t h = 1469598103934665603ull; // FNV offset basis
		const auto mix = [&h](const void* data, std::size_t n)
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (std::size_t i = 0; i < n; ++i)
			{
				h ^= bytes[i];
				h *= 1099511628211ull; // FNV prime
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

	// Source constructors for the common asset kinds.

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
