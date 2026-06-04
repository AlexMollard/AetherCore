#include "assets/GltfAsset.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include <BinaryFormats.hpp>

#include "io/FileSystem.hpp"
#include "io/FileGlobOptions.hpp"
#include "utils/Assert.hpp"
#include "utils/BinaryReader.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"
#include "utils/StringUtils.hpp"

namespace aether::assets
{
	namespace
	{
		std::vector<std::string> CollectSimilarMeshPaths(std::string_view meshPath, int maxSuggestions = 3)
		{
			const std::size_t ss = meshPath.find("://");
			if (ss == std::string_view::npos)
			{
				return {};
			}

			const std::string mount(meshPath.substr(0, ss));
			const std::filesystem::path rel(meshPath.substr(ss + 3));
			const std::string dirPattern = mount + "://" + rel.parent_path().generic_string() + "/*.mesh";

			auto result = io::FileSystem::Glob(dirPattern, {.recursive = false});
			if (!result.has_value() || result->empty())
			{
				return {};
			}

			const std::string targetFilename = rel.filename().generic_string();

			using Pair = std::pair<int, std::string>;
			auto cmp = [](const Pair& a, const Pair& b)
			{
				return a.first > b.first;
			};
			std::priority_queue<Pair, std::vector<Pair>, decltype(cmp)> pq(cmp);

			for (const auto& path: *result)
			{
				const std::string candidateFilename = std::filesystem::path(path).filename().generic_string();
				const int d = utils::Levenshtein(targetFilename, candidateFilename);
				pq.emplace(d, path);
				if (static_cast<int>(pq.size()) > maxSuggestions)
				{
					pq.pop();
				}
			}

			std::vector<std::string> suggestions;
			while (!pq.empty())
			{
				suggestions.push_back(std::move(pq.top().second));
				pq.pop();
			}
			std::reverse(suggestions.begin(), suggestions.end());
			return suggestions;
		}

		std::pair<std::string_view, std::string_view> SplitVfsPath(std::string_view vfsPath)
		{
			constexpr std::string_view kSeparator = "://";
			const std::size_t sep = vfsPath.find(kSeparator);
			if (sep == std::string_view::npos)
			{
				AE_ASSERT_ALWAYS(false, "Invalid VFS path (missing ://): " + std::string(vfsPath));
			}
			return {vfsPath.substr(0, sep), vfsPath.substr(sep + kSeparator.size())};
		}

		std::string ResolveRelativeVfsPath(std::string_view baseFilePath, std::string_view relativePath)
		{
			if (relativePath.starts_with("data:") || relativePath.find("://") != std::string_view::npos)
			{
				return std::string(relativePath);
			}

			auto [mount, baseRelative] = SplitVfsPath(baseFilePath);
			std::filesystem::path base = std::filesystem::path(std::string(baseRelative)).parent_path();
			std::filesystem::path resolved = (base / std::filesystem::path(std::string(relativePath))).lexically_normal();
			return std::string(mount) + "://" + resolved.generic_string();
		}

		// Derive the .mesh sibling of a GLTF/GLB VFS path.
		std::string DeriveMeshPath(std::string_view vfsPath)
		{
			const std::size_t ss = vfsPath.find("://");
			if (ss == std::string_view::npos)
			{
				return {};
			}
			const std::string mount(vfsPath.substr(0, ss));
			const std::filesystem::path rel(vfsPath.substr(ss + 3));
			return mount + "://" + (rel.parent_path() / rel.stem()).generic_string() + ".mesh";
		}

		// Derive the .skel path from a skin reference path stored in the mesh.
		std::string ResolveSkelPath(std::string_view meshVfsPath, std::string_view skinRefPath)
		{
			if (skinRefPath.empty())
			{
				return {};
			}
			std::string candidate = ResolveRelativeVfsPath(meshVfsPath, skinRefPath);
			// Try .skel extension
			if (io::FileSystem::Exists(candidate))
			{
				return candidate;
			}
			// Try replacing extension with .skel
			std::filesystem::path p(candidate);
			if (p.has_extension())
			{
				p.replace_extension(".skel");
				std::string skelPath = std::string(p.generic_string());
				// Re-insert mount
				auto [mount, rel] = SplitVfsPath(candidate);
				std::filesystem::path relP(rel);
				if (relP.has_extension())
				{
					relP.replace_extension(".skel");
					skelPath = std::string(mount) + "://" + relP.generic_string();
				}
				if (io::FileSystem::Exists(skelPath))
				{
					return skelPath;
				}
			}
			return {};
		}

		// Derive .animset path from mesh path (same directory, same stem).
		std::string DeriveAnimSetPath(std::string_view meshVfsPath)
		{
			const std::size_t ss = meshVfsPath.find("://");
			if (ss == std::string_view::npos)
			{
				return {};
			}
			const std::string mount(meshVfsPath.substr(0, ss));
			const std::filesystem::path rel(meshVfsPath.substr(ss + 3));
			std::filesystem::path animSetPath = rel.parent_path() / (rel.stem().string() + ".animset");
			std::string candidate = mount + "://" + animSetPath.generic_string();
			if (io::FileSystem::Exists(candidate))
			{
				return candidate;
			}
			return {};
		}

		// Unpack a packed RGBA8 uint32 into RGB floats (0-1 range).
		glm::vec3 UnpackColorRGBA8(uint32_t packed)
		{
			const float r = static_cast<float>((packed >> 0) & 0xFF) / 255.0f;
			const float g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
			const float b = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
			return glm::vec3(r, g, b);
		}

		// Load a .skel file and populate GltfSkin.
		GltfSkin LoadSkeleton(std::string_view skelPath)
		{
			GltfSkin skin;
			auto data = io::FileSystem::ReadFile(std::string(skelPath));
			if (!data.has_value())
			{
				return skin;
			}

			BinaryReader reader(*data);
			SkelHeaderDisk hdr = reader.Read<SkelHeaderDisk>();
			if (!CheckMagic(hdr))
			{
				return skin;
			}

			skin.name = std::string(skelPath.substr(skelPath.find_last_of('/') + 1));
			skin.joints.reserve(hdr.boneCount);
			skin.inverseBindMatrices.reserve(hdr.boneCount);

			// Read skeleton name (not used for runtime, skip)
			reader.Skip(hdr.nameLen);

			for (uint32_t i = 0; i < hdr.boneCount; ++i)
			{
				uint16_t boneNameLen = reader.Read<uint16_t>();
				std::string boneName = std::string(reinterpret_cast<const char*>(reader.Data()), boneNameLen);
				reader.Advance(boneNameLen);

				int32_t parentIndex = reader.Read<int32_t>();
				(void)parentIndex; // Parent info stored in nodes, not skin

				float ibm[16];
				reader.ReadRaw(ibm, sizeof(ibm));

				skin.joints.push_back(i);
				glm::mat4 ibmMat;
				std::memcpy(&ibmMat[0][0], ibm, sizeof(ibm));
				skin.inverseBindMatrices.push_back(ibmMat);
			}

			return skin;
		}

		// Load a .anim file and populate GltfAnimation.
		GltfAnimation LoadAnimation(std::string_view animPath)
		{
			GltfAnimation anim;
			auto data = io::FileSystem::ReadFile(std::string(animPath));
			if (!data.has_value())
			{
				return anim;
			}

			BinaryReader reader(*data);
			AnimHeaderDisk hdr = reader.Read<AnimHeaderDisk>();
			if (!CheckMagic(hdr))
			{
				return anim;
			}

			anim.name = std::string(reinterpret_cast<const char*>(reader.Data()), hdr.nameLen);
			reader.Advance(hdr.nameLen);
			anim.channels.reserve(hdr.channelCount);

			for (uint32_t ci = 0; ci < hdr.channelCount; ++ci)
			{
				ChannelHeaderDisk ch = reader.Read<ChannelHeaderDisk>();
				GltfAnimationChannel channel;
				channel.nodeIndex = ch.nodeIndex;

				switch (static_cast<AnimPathDisk>(ch.path))
				{
					case AnimPathDisk::Translation:
						channel.path = GltfAnimationPath::Translation;
						break;
					case AnimPathDisk::Rotation:
						channel.path = GltfAnimationPath::Rotation;
						break;
					case AnimPathDisk::Scale:
						channel.path = GltfAnimationPath::Scale;
						break;
					case AnimPathDisk::Weights:
						channel.path = GltfAnimationPath::Weights;
						break;
				}

				switch (static_cast<AnimInterpDisk>(ch.interp))
				{
					case AnimInterpDisk::Linear:
						channel.interpolation = GltfInterpolation::Linear;
						break;
					case AnimInterpDisk::Step:
						channel.interpolation = GltfInterpolation::Step;
						break;
					case AnimInterpDisk::CubicSpline:
						channel.interpolation = GltfInterpolation::CubicSpline;
						break;
				}

				channel.times.resize(ch.keyCount);
				reader.ReadRaw(channel.times.data(), ch.keyCount * sizeof(float));

				channel.values.resize(ch.keyCount);
				for (uint32_t k = 0; k < ch.keyCount; ++k)
				{
					float v4[4];
					reader.ReadRaw(v4, sizeof(v4));
					channel.values[k] = glm::vec4(v4[0], v4[1], v4[2], v4[3]);
				}

				anim.channels.push_back(std::move(channel));
			}

			return anim;
		}

		// Load a .material binary file and populate GltfMaterial.
		// Returns true on success, false if file not found or invalid.
		bool LoadMaterialBinary(std::string_view matPath, GltfMaterial& outMat)
		{
			auto data = io::FileSystem::ReadFile(std::string(matPath));
			if (!data.has_value())
			{
				return false;
			}

			BinaryReader reader(*data);
			MaterialHeaderDisk hdr = reader.Read<MaterialHeaderDisk>();
			if (!CheckMagic(hdr))
			{
				return false;
			}

			outMat.baseColorFactor = glm::vec4(hdr.baseColorFactor[0], hdr.baseColorFactor[1], hdr.baseColorFactor[2], hdr.baseColorFactor[3]);
			outMat.metallicFactor = hdr.metallicFactor;
			outMat.roughnessFactor = hdr.roughnessFactor;
			outMat.emissiveFactor = glm::vec3(hdr.emissiveFactor[0], hdr.emissiveFactor[1], hdr.emissiveFactor[2]);
			outMat.alphaCutoff = hdr.alphaCutoff;
			outMat.doubleSided = hdr.doubleSided != 0;
			outMat.alphaBlend = hdr.alphaBlend != 0;
			outMat.alphaMask = hdr.alphaMask != 0;

			// Read texture path entries.
			for (uint8_t t = 0; t < hdr.texturePathCount; ++t)
			{
				uint8_t type = reader.Read<uint8_t>();
				std::string texPath = reader.ReadString();

				// Resolve relative to the material file's directory.
				std::string resolvedPath = ResolveRelativeVfsPath(matPath, texPath);

				auto texType = static_cast<TextureTypeDisk>(type);
				switch (texType)
				{
					case TextureTypeDisk::BaseColor:
						outMat.albedoPath = std::move(resolvedPath);
						break;
					case TextureTypeDisk::Normal:
						outMat.normalPath = std::move(resolvedPath);
						break;
					case TextureTypeDisk::MetallicRoughness:
						outMat.metallicRoughnessPath = std::move(resolvedPath);
						break;
					case TextureTypeDisk::Occlusion:
						outMat.occlusionPath = std::move(resolvedPath);
						break;
					case TextureTypeDisk::Emissive:
						outMat.emissivePath = std::move(resolvedPath);
						break;
				}
			}

			return true;
		}

		// Load a .mesh file and populate GltfAsset.
		Expected<GltfAsset> LoadFromMesh(const std::vector<std::byte>& data, std::string_view meshVfsPath)
		{
			BinaryReader reader(data);
			MeshHeaderDisk hdr = reader.Read<MeshHeaderDisk>();

			if (std::memcmp(hdr.magic, MESH_MAGIC, 4) != 0)
			{
				AE_UNEXPECTED(AetherError::Asset("invalid mesh magic: " + std::string(meshVfsPath)));
			}
			if (hdr.version != MESH_VERSION)
			{
				AE_UNEXPECTED(AetherError::Asset("stale .mesh cache (version " + std::to_string(hdr.version) + ", expected " + std::to_string(MESH_VERSION) + ") for '" + std::string(meshVfsPath) + "'. Re-run AssetPacker."));
			}

			GltfAsset asset;

			// Read vertices.
			std::vector<DiskMeshVertex> diskVerts(hdr.vertexCount);
			reader.ReadRaw(diskVerts.data(), hdr.vertexCount * sizeof(DiskMeshVertex));

			// Read indices.
			std::vector<uint32_t> indices(hdr.indexCount);
			if (hdr.indexType == 0)
			{
				// uint16 indices
				std::vector<uint16_t> indices16(hdr.indexCount);
				reader.ReadRaw(indices16.data(), hdr.indexCount * sizeof(uint16_t));
				for (uint32_t i = 0; i < hdr.indexCount; ++i)
				{
					indices[i] = indices16[i];
				}
			}
			else
			{
				reader.ReadRaw(indices.data(), hdr.indexCount * sizeof(uint32_t));
			}

			// Read skin reference path.
			std::string skinRefPath;
			if (hdr.skinRefPathLen > 0)
			{
				skinRefPath = reader.ReadString();
			}

			// Read material paths.
			std::vector<std::string> matPaths(hdr.materialCount);
			for (uint32_t i = 0; i < hdr.materialCount; ++i)
			{
				matPaths[i] = reader.ReadString();
			}

			// Convert disk vertices to runtime vertices.
			GltfPrimitive prim;
			prim.nodeIndex = 0;
			prim.materialIndex = hdr.materialCount > 0 ? 0 : -1;
			prim.skinIndex = !skinRefPath.empty() ? 0 : -1;
			prim.vertices.resize(hdr.vertexCount);
			prim.indices = std::move(indices);

			for (uint32_t v = 0; v < hdr.vertexCount; ++v)
			{
				const DiskMeshVertex& src = diskVerts[v];
				Mesh::Vertex& dst = prim.vertices[v];
				dst.position = glm::vec3(src.position[0], src.position[1], src.position[2]);
				dst.normal = glm::vec3(src.normal[0], src.normal[1], src.normal[2]);
				dst.tangent = glm::vec4(src.tangent[0], src.tangent[1], src.tangent[2], src.tangent[3]);
				dst.uv = glm::vec2(src.uv[0], src.uv[1]);
				dst.color = UnpackColorRGBA8(src.color);
				dst.jointIndices = glm::uvec4(src.jointIndices[0], src.jointIndices[1], src.jointIndices[2], src.jointIndices[3]);
				dst.jointWeights = glm::vec4(src.jointWeights[0], src.jointWeights[1], src.jointWeights[2], src.jointWeights[3]);
			}

			asset.primitives.push_back(std::move(prim));

			// Create a root node referencing the primitive.
			GltfNode rootNode;
			rootNode.name = "root";
			rootNode.meshIndex = 0;
			rootNode.skinIndex = prim.skinIndex;
			asset.nodes.push_back(std::move(rootNode));

			// Load skeleton if present.
			if (!skinRefPath.empty())
			{
				std::string skelPath = ResolveSkelPath(meshVfsPath, skinRefPath);
				if (!skelPath.empty())
				{
					GltfSkin skin = LoadSkeleton(skelPath);
					if (!skin.joints.empty())
					{
						asset.skins.push_back(std::move(skin));
					}
				}
			}

			// Load materials from binary .material files.
			for (uint32_t i = 0; i < hdr.materialCount; ++i)
			{
				GltfMaterial mat;
				mat.name = "material_" + std::to_string(i);

				std::string matFullPath = ResolveRelativeVfsPath(meshVfsPath, matPaths[i]);

				// Try binary .material first, then fall back to TOML folder.
				if (!LoadMaterialBinary(matFullPath, mat))
				{
					// Try as a folder with properties.toml
					std::string tomlPath = matFullPath + "/properties.toml";
					if (io::FileSystem::Exists(tomlPath))
					{
						// TOML materials are loaded at runtime by AssetManager, not here.
						// Store the folder path so AssetManager can resolve it later.
						mat.name = matPaths[i];
					}
				}
				else
				{
					mat.name = matPaths[i];
				}

				asset.materials.push_back(std::move(mat));
			}

			// Load animations from .animset if present.
			std::string animSetPath = DeriveAnimSetPath(meshVfsPath);
			if (!animSetPath.empty())
			{
				auto animSetData = io::FileSystem::ReadFile(animSetPath);
				if (animSetData.has_value())
				{
					BinaryReader animSetReader(*animSetData);
					AnimSetHeaderDisk asetHdr = animSetReader.Read<AnimSetHeaderDisk>();
					if (CheckMagic(asetHdr))
					{
						// Resolve .anim paths relative to the animset file's directory.
						std::string animDir = animSetPath.substr(0, animSetPath.find_last_of('/') + 1);

						for (uint32_t i = 0; i < asetHdr.animCount; ++i)
						{
							std::string animRelPath = animSetReader.ReadString();
							std::string animFullPath = animDir + animRelPath;
							if (io::FileSystem::Exists(animFullPath))
							{
								GltfAnimation anim = LoadAnimation(animFullPath);
								if (!anim.name.empty())
								{
									asset.animations.push_back(std::move(anim));
								}
							}
						}
					}
				}
			}

			return asset;
		}
	} // namespace

	Expected<GltfAsset> GltfAsset::LoadFromVfsPath(std::string_view path)
	{
		AE_PROFILE_ZONE_N("GltfAsset::Load");
		AE_PROFILE_SET_ZONE_NAME(path.data());

		const std::string vfsPath(path);

		// Derive the .mesh path - try the exact path given first (caller may already
		// pass a .mesh path), then fall back to replacing the extension.
		auto TryMesh = [&](const std::string& meshPath) -> bool
		{
			return !meshPath.empty() && io::FileSystem::Exists(meshPath);
		};

		std::string meshPath = vfsPath;
		if (!TryMesh(meshPath))
		{
			meshPath = DeriveMeshPath(vfsPath);
		}

		if (!TryMesh(meshPath))
		{
			std::string msg = "packed .mesh not found for '" + vfsPath + "'. Run AssetPacker to generate it.";
			auto suggestions = CollectSimilarMeshPaths(meshPath);
			if (!suggestions.empty())
			{
				msg += " Did you mean: ";
				for (std::size_t i = 0; i < suggestions.size(); ++i)
				{
					if (i > 0)
					{
						msg += ", ";
					}
					msg += "'" + suggestions[i] + "'";
				}
				msg += "?";
			}
			AE_UNEXPECTED(AetherError::Asset(std::move(msg)));
		}

		std::vector<std::byte> meshData;
		{
			AE_PROFILE_ZONE_N("GltfAsset::LoadMesh");
			AE_EXPECT_OR_THROW(data, io::FileSystem::ReadFile(meshPath));
			meshData = std::move(data);
		}
		return LoadFromMesh(meshData, meshPath);
	}

	Expected<GltfAsset> GltfAsset::LoadFromMemory(std::vector<std::byte> meshData, std::string_view debugPath)
	{
		return LoadFromMesh(meshData, debugPath);
	}

	std::string GltfAsset::ResolveMeshPath(std::string_view vfsPath)
	{
		const std::size_t ss = vfsPath.find("://");
		if (ss == std::string_view::npos)
		{
			return {};
		}
		const std::string mount(vfsPath.substr(0, ss));
		const std::filesystem::path rel(vfsPath.substr(ss + 3));
		return mount + "://" + (rel.parent_path() / rel.stem()).generic_string() + ".mesh";
	}
} // namespace aether::assets
