#include "assets/StreamingDecode.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <utility>

#include <stb_image.h>

#include "assets/SpriteAtlasAsset.hpp"
#include "assets/TileMapAsset.hpp"
#include "assets/TileSetAsset.hpp"
#include "audio/AudioSubsystem.hpp"
#include "io/FileSystem.hpp"
#include "ui/FontRegistry.hpp"

namespace aether::assets
{
	namespace
	{
		Expected<void> Reserve(const std::function<bool(std::size_t)>& reserve, std::size_t amount)
		{
			if (!reserve || !reserve(amount))
			{
				AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));
			}
			return {};
		}

		Expected<std::vector<std::byte>> Read(std::string_view path, const std::function<bool(std::size_t)>& reserve)
		{
			AE_TRY_VOID(Reserve(reserve, 0));
			return io::FileSystem::ReadFileReserved(path, reserve);
		}

		Expected<std::string> PreparedPath(std::string_view reference)
		{
			std::string path = io::FileSystem::ResolveAssetPath(reference);
			if (path.empty())
			{
				AE_UNEXPECTED(AetherError::Asset("Unresolved texture: " + std::string(reference)));
			}
			path = Texture::ResolveTexturePath(path);
			return path;
		}

		Expected<void> AddTexture(StreamingDecodeResult& result, std::string_view reference, TextureColorSpace colorSpace, const std::function<bool(std::size_t)>& reserve)
		{
			AE_TRY_VOID(Reserve(reserve, 0));
			if (reference.empty())
			{
				return {};
			}
			AE_TRY(resolved, PreparedPath(reference));
			std::string path = std::move(*resolved);
			for (const auto& texture: result.textures)
			{
				if (texture.path == path && texture.colorSpace == colorSpace)
				{
					return {};
				}
			}
			std::string dataPath = path;
			if (colorSpace == TextureColorSpace::Linear && dataPath.ends_with(".texture"))
			{
				const auto variant = dataPath.substr(0, dataPath.size() - 8) + ".linear.texture";
				if (io::FileSystem::Exists(variant))
				{
					dataPath = variant;
				}
			}
			AE_TRY(data, Read(dataPath, reserve));
			std::size_t decodedSize = data->size();
			if (data->size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()))
			{
				int width = 0, height = 0, channels = 0;
				if (stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(data->data()), static_cast<int>(data->size()), &width, &height, &channels) && width > 0 && height > 0)
				{
					const auto w = static_cast<std::size_t>(width), h = static_cast<std::size_t>(height);
					if (w > std::numeric_limits<std::size_t>::max() / h / 8)
					{
						AE_UNEXPECTED(AetherError::Asset("Streaming texture too large: " + path));
					}
					decodedSize = w * h * 8; // stb pixels and the retained copy coexist during decode
				}
			}
			AE_TRY_VOID(Reserve(reserve, decodedSize));
			AE_TRY_VOID(Reserve(reserve, 0));
			AE_TRY(prepared, Texture::Decode(*data, dataPath));
			result.byteSize += prepared->data.size();
			result.dependencies.push_back(path);
			result.textures.push_back({std::move(path), colorSpace, std::move(*prepared)});
			return {};
		}

		template<class Material>
		Expected<void> AddMaterialTextures(StreamingDecodeResult& result, Material& material, const std::function<bool(std::size_t)>& reserve)
		{
			auto add = [&](std::string& path, TextureColorSpace space) -> Expected<void>
			{
				if (path.empty())
				{
					return {};
				}
				AE_TRY_VOID(AddTexture(result, path, space, reserve));
				AE_TRY(resolved, PreparedPath(path));
				path = std::move(*resolved);
				return {};
			};
			AE_TRY_VOID(add(material.albedoPath, TextureColorSpace::Srgb));
			AE_TRY_VOID(add(material.normalPath, TextureColorSpace::Linear));
			AE_TRY_VOID(add(material.metallicRoughnessPath, TextureColorSpace::Linear));
			AE_TRY_VOID(add(material.occlusionPath, TextureColorSpace::Linear));
			AE_TRY_VOID(add(material.emissivePath, TextureColorSpace::Srgb));
			return {};
		}
	} // namespace

	static Expected<StreamingDecodeResult> DecodeStreamingAssetImpl(std::string_view reference, const std::function<bool(std::size_t)>& reserve)
	{
		AE_TRY_VOID(Reserve(reserve, 0));
		std::string path = io::FileSystem::ResolveAssetPath(reference);
		if (path.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Unresolved streaming asset: " + std::string(reference)));
		}
		StreamingDecodeResult result;
		if (path.ends_with(".mesh") || path.ends_with(".gltf") || path.ends_with(".glb") || path.ends_with(".fbx") || path.ends_with(".obj"))
		{
			if (!path.ends_with(".mesh"))
			{
				path = GltfAsset::ResolveMeshPath(path);
			}
			if (path.empty())
			{
				AE_UNEXPECTED(AetherError::Asset("Cannot resolve packed model: " + std::string(reference)));
			}
			AE_TRY(data, Read(path, reserve));
			AE_TRY_VOID(Reserve(reserve, 0));
			AE_TRY(model, GltfAsset::LoadFromMemory(*data, path, reserve));
			for (const auto& primitive: model->primitives)
			{
				result.byteSize += primitive.vertices.size() * sizeof(MeshVertex) + primitive.indices.size() * sizeof(std::uint32_t);
			}
			for (auto& material: model->materials)
			{
				AE_TRY_VOID(Reserve(reserve, 0));
				AE_TRY_VOID(AddMaterialTextures(result, material, reserve));
				auto addIndexed = [&](std::int32_t index, TextureColorSpace space) -> Expected<void>
				{
					if (index < 0)
					{
						return {};
					}
					if (static_cast<std::size_t>(index) >= model->textures.size())
					{
						AE_UNEXPECTED(AetherError::Asset("Invalid model texture index: " + path));
					}
					const auto image = model->textures[static_cast<std::size_t>(index)].imageIndex;
					if (image < 0 || static_cast<std::size_t>(image) >= model->images.size())
					{
						AE_UNEXPECTED(AetherError::Asset("Invalid model image index: " + path));
					}
					const auto& uri = model->images[static_cast<std::size_t>(image)].uri;
					if (uri.empty() || uri.starts_with("data:"))
					{
						AE_UNEXPECTED(AetherError::Asset("Unsupported embedded model image: " + path));
					}
					return AddTexture(result, io::FileSystem::ResolveRelative(path, uri), space, reserve);
				};
				AE_TRY_VOID(addIndexed(material.baseColorTexture, TextureColorSpace::Srgb));
				AE_TRY_VOID(addIndexed(material.normalTexture, TextureColorSpace::Linear));
				AE_TRY_VOID(addIndexed(material.metallicRoughnessTexture, TextureColorSpace::Linear));
				AE_TRY_VOID(addIndexed(material.occlusionTexture, TextureColorSpace::Linear));
				AE_TRY_VOID(addIndexed(material.emissiveTexture, TextureColorSpace::Srgb));
			}
			result.model = std::move(*model);
			return result;
		}
		if (path.ends_with(".material") || path.ends_with(".material.toml"))
		{
			AE_TRY(data, Read(path, reserve));
			AE_TRY_VOID(Reserve(reserve, 0));
			AE_TRY(material, MaterialSerializer::LoadBytes(path, *data, reserve));
			AE_TRY_VOID(AddMaterialTextures(result, *material, reserve));
			result.byteSize += data->size();
			result.material = std::move(*material);
			return result;
		}
		if (path.ends_with(".png") || path.ends_with(".jpg") || path.ends_with(".jpeg") || path.ends_with(".tga") || path.ends_with(".bmp") || path.ends_with(".dds") || path.ends_with(".texture"))
		{
			AE_TRY_VOID(AddTexture(result, path, TextureColorSpace::Srgb, reserve));
			return result;
		}
		if (path.ends_with(".ttf") || path.ends_with(".otf"))
		{
			path = path.substr(0, path.find_last_of('.')) + ".fontcurves";
		}
		if (path.ends_with(".spriteatlas.toml") || path.ends_with(".spriteanim.toml") || path.ends_with(".tileset.toml") || path.ends_with(".tilemap") || path.ends_with(".tiles") || path.ends_with(".fontcurves") || path.ends_with(".wav")
		        || path.ends_with(".ogg") || path.ends_with(".mp3") || path.ends_with(".flac"))
		{
			AE_TRY(data, Read(path, reserve));
			AE_TRY_VOID(Reserve(reserve, 0));
			result.byteSize = data->size();
			result.bytes = std::move(*data);
			const std::string_view content(reinterpret_cast<const char*>(result.bytes.data()), result.bytes.size());
			if (path.ends_with(".spriteatlas.toml"))
			{
				AE_TRY(atlas, SpriteAtlasAsset::LoadBytes(content, reserve));
				if (!atlas->texturePath.empty())
				{
					atlas->texturePath = io::FileSystem::ResolveRelative(path, atlas->texturePath);
					AE_TRY_VOID(AddTexture(result, atlas->texturePath, TextureColorSpace::Srgb, reserve));
					AE_TRY(resolved, PreparedPath(atlas->texturePath));
					atlas->texturePath = std::move(*resolved);
				}
				result.atlas = std::move(*atlas);
			}
			else if (path.ends_with(".spriteanim.toml"))
			{
				AE_TRY(animation, SpriteAnimationAsset::LoadBytes(content, reserve));
				if (!animation->atlasPath.empty())
				{
					animation->atlasPath = io::FileSystem::ResolveRelative(path, animation->atlasPath);
					result.dependencies.push_back(animation->atlasPath);
				}
				result.animation = std::move(*animation);
			}
			else if (path.ends_with(".tileset.toml"))
			{
				AE_TRY(tiles, TileSetAsset::LoadBytes(content, reserve));
				for (auto& tile: tiles->tiles)
				{
					if (!tile.atlasPath.empty())
					{
						tile.atlasPath = io::FileSystem::ResolveRelative(path, tile.atlasPath);
						result.dependencies.push_back(tile.atlasPath);
					}
				}
				result.tileSet = std::move(*tiles);
			}
			else if (path.ends_with(".tiles") || path.ends_with(".tilemap"))
			{
				AE_TRY(map, TileMapAsset::LoadBytes(content, reserve));
				if (!map->tileSetPath.empty())
				{
					map->tileSetPath = io::FileSystem::ResolveRelative(path, map->tileSetPath);
					result.dependencies.push_back(map->tileSetPath);
				}
				result.tileMap = std::move(*map);
			}
			else if (path.ends_with(".fontcurves"))
			{
				AE_TRY(font, ui::FontRegistry::ParseBytes(path, result.bytes, reserve));
				result.font = std::move(*font);
			}
			else
			{
				AE_TRY_VOID(audio::ValidateStreamedClip(result.bytes, reserve));
			}
			return result;
		}
		AE_UNEXPECTED(AetherError::Asset("Unsupported streaming asset format: " + path));
	}

	Expected<StreamingDecodeResult> DecodeStreamingAsset(std::string_view path, const std::function<bool(std::size_t)>& reserve, const std::function<bool()>& cancelled)
	{
		auto guardedReserve = [&](std::size_t amount)
		{
			return !(cancelled && cancelled()) && (amount == 0 || (reserve && reserve(amount)));
		};
		auto result = DecodeStreamingAssetImpl(path, guardedReserve);
		if (cancelled && cancelled())
		{
			AE_UNEXPECTED(AetherError::Asset("Streaming decode cancelled"));
		}
		return result;
	}
} // namespace aether::assets
