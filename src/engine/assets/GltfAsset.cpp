#include "assets/GltfAsset.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <BinaryFormats.hpp>
#include "assets/ExternalAnimation.hpp"

#include "io/FileSystem.hpp"
#include "io/FileGlobOptions.hpp"
#include "utils/Assert.hpp"
#include "utils/BinaryReader.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/StringUtils.hpp"

namespace aether::assets
{
	namespace
	{
		// Hard ceilings so a corrupt .mesh/.skel/.anim header can never drive a huge
		// allocation (same policy as TileMapAsset's kMax* limits).
		constexpr uint32_t kMaxMeshVertices = 1u << 23;
		constexpr uint32_t kMaxMeshIndices = 1u << 25;
		constexpr uint32_t kMaxSubMeshes = 1u << 13;
		constexpr uint32_t kMaxMaterials = 1u << 11;
		constexpr uint32_t kMaxSkinRefPathLen = 4096;
		constexpr uint32_t kMaxBones = 4096;

		struct ParseBudget
		{
			const std::function<bool(std::size_t)>* reserve = nullptr;
			bool rejected = false;

			bool Add(std::size_t bytes, std::size_t multiplier = 1)
			{
				if (!reserve) return true;
				if (multiplier == 0) return !rejected;
				if (bytes > std::numeric_limits<std::size_t>::max() / multiplier ||
				    (bytes && (!*reserve || !(*reserve)(bytes * multiplier))))
					rejected = true;
				return !rejected;
			}

			Expected<std::vector<std::byte>> Read(std::string_view path)
			{
				if (!reserve) return io::FileSystem::ReadFile(path);
				auto data = io::FileSystem::ReadFileReserved(path, [this](std::size_t bytes) { return Add(bytes); });
				// Bound all parser-owned structures and strings derived from the file.
				if (data && !Add(data->size(), 32))
					AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));
				return data;
			}
		};

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
				if (std::cmp_greater(pq.size(), maxSuggestions))
				{
					pq.pop();
				}
			}

			std::vector<std::string> suggestions;
			while (!pq.empty())
			{
				suggestions.push_back(pq.top().second);
				pq.pop();
			}
			std::ranges::reverse(suggestions);
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
			if (relativePath.starts_with("data:") || relativePath.contains("://"))
			{
				return std::string(relativePath);
			}

			auto [mount, baseRelative] = SplitVfsPath(baseFilePath);

			const std::filesystem::path relP = std::filesystem::path(std::string(relativePath));
			if (!relP.is_absolute() && !relativePath.contains(".."))
			{
				return std::string(mount) + "://" + relP.lexically_normal().generic_string();
			}

			const std::filesystem::path base = std::filesystem::path(std::string(baseRelative)).parent_path();
			const std::filesystem::path resolved = (base / relP).lexically_normal();
			return std::string(mount) + "://" + resolved.generic_string();
		}

		std::string ResolveSkelPath(std::string_view meshVfsPath, std::string_view skinRefPath)
		{
			if (skinRefPath.empty())
			{
				return {};
			}
			std::string candidate = ResolveRelativeVfsPath(meshVfsPath, skinRefPath);
			if (io::FileSystem::Exists(candidate))
			{
				return candidate;
			}
			std::filesystem::path p(candidate);
			if (p.has_extension())
			{
				p.replace_extension(".skel");
				std::string skelPath = std::string(p.generic_string());
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

		std::string DeriveAnimSetPath(std::string_view meshVfsPath)
		{
			const std::size_t ss = meshVfsPath.find("://");
			if (ss == std::string_view::npos)
			{
				return {};
			}
			const std::string mount(meshVfsPath.substr(0, ss));
			const std::filesystem::path rel(meshVfsPath.substr(ss + 3));
			const std::filesystem::path animSetPath = rel.parent_path() / (rel.stem().string() + ".animset");
			std::string candidate = mount + "://" + animSetPath.generic_string();
			if (io::FileSystem::Exists(candidate))
			{
				return candidate;
			}
			return {};
		}

		glm::vec3 UnpackColorRGBA8(uint32_t packed)
		{
			const float r = static_cast<float>((packed >> 0) & 0xFF) / 255.0f;
			const float g = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
			const float b = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
			return {r, g, b};
		}

		void DecomposeTransform(const glm::mat4& mat, glm::vec3& outT, glm::quat& outR, glm::vec3& outS)
		{
			outT = glm::vec3(mat[3][0], mat[3][1], mat[3][2]);

			glm::vec3 col0(mat[0][0], mat[0][1], mat[0][2]);
			glm::vec3 col1(mat[1][0], mat[1][1], mat[1][2]);
			glm::vec3 col2(mat[2][0], mat[2][1], mat[2][2]);

			outS.x = glm::length(col0);
			outS.y = glm::length(col1);
			outS.z = glm::length(col2);

			if (outS.x > 0.0001f)
			{
				col0 /= outS.x;
			}
			if (outS.y > 0.0001f)
			{
				col1 /= outS.y;
			}
			if (outS.z > 0.0001f)
			{
				col2 /= outS.z;
			}

			const glm::mat3 rotMat(col0, col1, col2);
			outR = glm::quat_cast(rotMat);
		}

		bool LoadSkeleton(std::string_view skelPath, GltfAsset& asset, GltfSkin& outSkin, ParseBudget& budget)
		{
			auto data = budget.Read(skelPath);
			if (!data.has_value())
			{
				AE_WARN(LogCategory::Engine, "Skeleton file not found: {}", skelPath);
				return false;
			}

			BinaryReader reader(*data);
			SkelHeaderDisk hdr;
			// TryRead, not Read: see LoadFromMesh's own comment on why - SkelHeaderDisk
			// also default-initialises magic/version to its own valid values.
			if (!reader.TryRead(hdr) || !CheckMagic(hdr) || hdr.version != SKEL_VERSION)
			{
				AE_WARN(LogCategory::Engine, "Invalid skeleton magic: {}", skelPath);
				return false;
			}

			// Name length + parent index + inverse bind matrix is the smallest possible bone
			// entry; bounding boneCount by that (and by a hard ceiling) keeps a corrupt or
			// truncated header from driving a huge allocation.
			constexpr uint32_t kMinBoneEntryBytes = 2 + 4 + 64;
			if (hdr.boneCount > kMaxBones || static_cast<uint64_t>(hdr.boneCount) * kMinBoneEntryBytes > reader.Remaining())
			{
				AE_WARN(LogCategory::Engine, "Skeleton bone count {} out of range ({} bytes left): {}", hdr.boneCount, reader.Remaining(), skelPath);
				return false;
			}

			AE_VERBOSE(LogCategory::Engine, "Loading skeleton '{}': {} bones, hash={}", skelPath, hdr.boneCount, hdr.skeletonHash);

			outSkin.name = std::string(skelPath.substr(skelPath.find_last_of('/') + 1));
			outSkin.joints.reserve(hdr.boneCount);
			outSkin.inverseBindMatrices.reserve(hdr.boneCount);

			reader.Skip(hdr.nameLen);

			const auto boneNodeOffset = static_cast<uint32_t>(asset.nodes.size());

			struct BoneData
			{
				std::string name;
				int32_t parentIndex;
				float ibm[16];
			};

			std::vector<BoneData> bones(hdr.boneCount);

			for (uint32_t i = 0; i < hdr.boneCount; ++i)
			{
				auto boneNameLen = reader.Read<uint16_t>();
				if (!reader.CanRead(boneNameLen))
				{
					AE_WARN(LogCategory::Engine, "Truncated bone name in skeleton: {}", skelPath);
					return false;
				}
				bones[i].name = std::string(reinterpret_cast<const char*>(reader.Data()), boneNameLen);
				reader.Advance(boneNameLen);
				bones[i].parentIndex = reader.Read<int32_t>();
				// Parents may point forward (the packer sorts bones by name, so a parent can
				// sort after its child), but never past the bone table or at the bone itself.
				if (bones[i].parentIndex >= static_cast<int32_t>(hdr.boneCount) || bones[i].parentIndex == static_cast<int32_t>(i))
				{
					AE_WARN(LogCategory::Engine, "Bone {} ('{}') has invalid parent index {}: {}", i, bones[i].name, bones[i].parentIndex, skelPath);
					return false;
				}
				reader.ReadRaw(bones[i].ibm, sizeof(bones[i].ibm));
			}

			std::vector<glm::mat4> boneWorld(hdr.boneCount);
			for (uint32_t i = 0; i < hdr.boneCount; ++i)
			{
				glm::mat4 ibmMat;
				std::memcpy(&ibmMat[0][0], bones[i].ibm, sizeof(bones[i].ibm));
				boneWorld[i] = glm::inverse(ibmMat);
				outSkin.inverseBindMatrices.push_back(ibmMat);
			}

			for (uint32_t i = 0; i < hdr.boneCount; ++i)
			{
				GltfNode boneNode;
				boneNode.name = bones[i].name;

				if (bones[i].parentIndex < 0)
				{
					boneNode.parentIndex = 0;
				}
				else
				{
					boneNode.parentIndex = static_cast<int32_t>(boneNodeOffset + bones[i].parentIndex);
				}

				glm::mat4 localMat = boneWorld[i];
				if (bones[i].parentIndex >= 0)
				{
					localMat = glm::inverse(boneWorld[static_cast<std::size_t>(bones[i].parentIndex)]) * localMat;
				}
				DecomposeTransform(localMat, boneNode.translation, boneNode.rotation, boneNode.scale);

				asset.nodes.push_back(std::move(boneNode));

				outSkin.joints.push_back(boneNodeOffset + i);
			}

			AE_VERBOSE(LogCategory::Engine, "Skeleton loaded: {} bones, nodeOffset={}", hdr.boneCount, boneNodeOffset);
			for (uint32_t i = 0; i < hdr.boneCount && i < 3; ++i)
			{
				AE_VERBOSE(LogCategory::Engine,
				        "  Bone[{}]: '{}' parentIdx={} t=({:.1f},{:.1f},{:.1f}) r=({:.3f},{:.3f},{:.3f},{:.3f})",
				        i,
				        bones[i].name,
				        bones[i].parentIndex,
				        asset.nodes[boneNodeOffset + i].translation.x,
				        asset.nodes[boneNodeOffset + i].translation.y,
				        asset.nodes[boneNodeOffset + i].translation.z,
				        asset.nodes[boneNodeOffset + i].rotation.x,
				        asset.nodes[boneNodeOffset + i].rotation.y,
				        asset.nodes[boneNodeOffset + i].rotation.z,
				        asset.nodes[boneNodeOffset + i].rotation.w);
			}
			return true;
		}

		GltfAnimation LoadAnimation(std::string_view animPath, ParseBudget& budget)
		{
			auto data = budget.Read(animPath);
			if (!data)
			{
				AE_WARN(LogCategory::Engine, "Animation file not found: {}", animPath);
				return {};
			}
			auto animation = ParseExternalAnimation(*data);
			if (!animation)
			{
				AE_WARN(LogCategory::Engine, "Invalid or truncated animation: {}", animPath);
				return {};
			}
			return std::move(*animation);
		}

		std::unordered_map<std::string, uint32_t> BuildBoneNameMap(const GltfAsset& asset, uint32_t boneNodeOffset, uint32_t jointCount)
		{
			std::unordered_map<std::string, uint32_t> nameMap;
			for (uint32_t i = 0; i < jointCount; ++i)
			{
				const uint32_t nodeIdx = boneNodeOffset + i;
				if (nodeIdx < asset.nodes.size())
				{
					const auto& nodeName = asset.nodes[nodeIdx].name;
					if (!nodeName.empty())
					{
						nameMap[nodeName] = nodeIdx;
					}
				}
			}
			return nameMap;
		}

		GltfAnimation RemapAnimationByBoneName(const GltfAnimation& anim, const std::unordered_map<std::string, uint32_t>& boneNameMap)
		{
			GltfAnimation result;
			result.name = anim.name + " (remapped)";
			result.channels.reserve(anim.channels.size());

			for (const auto& ch: anim.channels)
			{
				if (ch.boneName.empty())
				{
					continue;
				}

				auto it = boneNameMap.find(ch.boneName);
				if (it != boneNameMap.end())
				{
					GltfAnimationChannel remapped = ch;
					remapped.nodeIndex = it->second;
					remapped.boneName.clear();
					result.channels.push_back(remapped);
				}
			}

			return result;
		}

		// Cross-skeleton (retargeted) AnimSet loading: complete, but nothing calls it yet - the loader
		// below only handles the same-skeleton case. Left in place deliberately; see BuildBoneNameMap.
		[[maybe_unused]] bool TryLoadCrossSkeletonAnimations(const std::string& animSetPath, GltfAsset& asset, uint32_t boneNodeOffset, uint32_t jointCount)
		{
			if (animSetPath.empty() || !io::FileSystem::Exists(animSetPath))
			{
				return false;
			}

			auto animSetData = io::FileSystem::ReadFile(animSetPath);
			if (!animSetData.has_value())
			{
				return false;
			}

			BinaryReader reader(*animSetData);
			AnimSetHeaderDisk asetHdr;
			if (!reader.TryRead(asetHdr) || !CheckMagic(asetHdr) || asetHdr.version != ASET_VERSION)
			{
				return false;
			}

			if (asetHdr.skeletonHash != 0)
			{
				return false;
			}

			AE_VERBOSE(LogCategory::Engine, "  Cross-skeleton AnimSet: {} animations", asetHdr.animCount);

			const std::string mountRoot = animSetPath.substr(0, animSetPath.find("://") + 3);

			auto boneNameMap = BuildBoneNameMap(asset, boneNodeOffset, jointCount);
			if (boneNameMap.empty())
			{
				AE_WARN(LogCategory::Engine, "  Cross-skeleton: no bones found for remapping");
				return false;
			}

			bool anyLoaded = false;
			for (uint32_t i = 0; i < asetHdr.animCount; ++i)
			{
				const std::string animRelPath = reader.ReadString();
				std::string animFullPath = mountRoot + animRelPath;

				if (!io::FileSystem::Exists(animFullPath))
				{
					AE_WARN(LogCategory::Engine, "  Cross-skeleton animation file not found: {}", animFullPath);
					continue;
				}

				ParseBudget budget;
				GltfAnimation anim = LoadAnimation(animFullPath, budget);
				if (!anim.name.empty() && !anim.channels.empty())
				{
					GltfAnimation remapped = RemapAnimationByBoneName(anim, boneNameMap);
					if (!remapped.channels.empty())
					{
						AE_VERBOSE(LogCategory::Engine, "    Remapped '{}': {} -> {} channels", anim.name, anim.channels.size(), remapped.channels.size());
						asset.animations.push_back(std::move(remapped));
						anyLoaded = true;
					}
				}
			}

			return anyLoaded;
		}

		bool LoadMaterialBinary(std::string_view matPath, GltfMaterial& outMat, ParseBudget& budget)
		{
			auto data = budget.Read(matPath);
			if (!data.has_value())
			{
				return false;
			}

			BinaryReader reader(*data);
			MaterialHeaderDisk hdr;
			// TryRead, not Read: see LoadFromMesh's own comment on why -
			// MaterialHeaderDisk also default-initialises magic/version to its own
			// valid values.
			if (!reader.TryRead(hdr) || !CheckMagic(hdr) || hdr.version < 1 || hdr.version > MATL_VERSION)
			{
				return false;
			}
			if (!budget.Add(matPath.size(), static_cast<std::size_t>(hdr.texturePathCount) * 4 + 4))
				return false;

			outMat.baseColorFactor = glm::vec4(hdr.baseColorFactor[0], hdr.baseColorFactor[1], hdr.baseColorFactor[2], hdr.baseColorFactor[3]);
			outMat.metallicFactor = hdr.metallicFactor;
			outMat.roughnessFactor = hdr.roughnessFactor;
			outMat.emissiveFactor = glm::vec3(hdr.emissiveFactor[0], hdr.emissiveFactor[1], hdr.emissiveFactor[2]);
			outMat.alphaCutoff = hdr.alphaCutoff;
			outMat.doubleSided = hdr.doubleSided != 0;
			outMat.alphaBlend = hdr.alphaBlend != 0;
			outMat.additiveBlend = hdr.alphaBlend == 2;
			outMat.alphaMask = hdr.alphaMask != 0;
			outMat.modulateVertexColor = hdr.modulateVertexColor != 0;
			outMat.foliage = hdr.foliage != 0;
			outMat.bakedLighting = (hdr.bakedLighting & kMaterialDiskBaked) != 0;
			outMat.sky = (hdr.bakedLighting & kMaterialDiskSky) != 0;
			outMat.lightShaft = (hdr.bakedLighting & kMaterialDiskLightShaft) != 0;
			outMat.gem = (hdr.bakedLighting & kMaterialDiskGem) != 0;
			outMat.sheen = (hdr.bakedLighting & kMaterialDiskSheen) != 0;
			outMat.water = (hdr.bakedLighting & kMaterialDiskWater) != 0;
			outMat.character = (hdr.bakedLighting & kMaterialDiskCharacter) != 0;
			outMat.objectLit = hdr.objectLit != 0;
			outMat.uvScroll = glm::vec2(hdr.uvScroll[0], hdr.uvScroll[1]);

			for (uint8_t t = 0; t < hdr.texturePathCount; ++t)
			{
				auto type = reader.Read<uint8_t>();
				const std::string texPath = reader.ReadString();
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

			outMat.shaderVfsPath = reader.ReadString();

			return true;
		}

		Expected<GltfAsset> LoadFromMesh(const std::vector<std::byte>& data, std::string_view meshVfsPath, ParseBudget& budget)
		{
			BinaryReader reader(data);
			MeshHeaderDisk hdr;
			// TryRead, not Read: MeshHeaderDisk default-initialises magic/version to
			// its OWN valid values (writer convenience), so Read<T>()'s "return T{}
			// on truncation" would make an empty/truncated file pass this exact check.
			if (!reader.TryRead(hdr) || std::memcmp(hdr.magic, MESH_MAGIC, 4) != 0)
			{
				AE_UNEXPECTED(AetherError::Asset("invalid mesh magic: " + std::string(meshVfsPath)));
			}
			if (hdr.version != 3 && hdr.version != MESH_VERSION)
			{
				AE_UNEXPECTED(AetherError::Asset("stale .mesh cache (version " + std::to_string(hdr.version) + ", expected " + std::to_string(MESH_VERSION) + ") for '" + std::string(meshVfsPath) + "'. Re-run AssetPacker."));
			}

			// Hard ceilings plus a byte-accounting check: the vertex and index blobs the
			// loader consumes must fit in what is left of the file, so a corrupt or truncated
			// header can neither drive a huge allocation nor walk past the buffer.
			if (hdr.vertexCount > kMaxMeshVertices || hdr.indexCount > kMaxMeshIndices || hdr.subMeshCount > kMaxSubMeshes || hdr.materialCount > kMaxMaterials || hdr.skinRefPathLen > kMaxSkinRefPathLen)
			{
				AE_WARN(LogCategory::Engine, "Mesh header counts out of range: {} verts, {} indices, {} submeshes, {} materials, skinRefLen={} for '{}'", hdr.vertexCount, hdr.indexCount, hdr.subMeshCount, hdr.materialCount, hdr.skinRefPathLen, meshVfsPath);
				AE_UNEXPECTED(AetherError::Asset("mesh header counts out of range: " + std::string(meshVfsPath)));
			}
			const auto vertexBytes = static_cast<uint64_t>(hdr.vertexCount) * sizeof(DiskMeshVertex);
			const auto indexBytes = static_cast<uint64_t>(hdr.indexCount) * ((hdr.indexType == 0) ? sizeof(uint16_t) : sizeof(uint32_t));
			if (static_cast<uint64_t>(reader.Remaining()) < vertexBytes + indexBytes)
			{
				AE_WARN(LogCategory::Engine, "Truncated mesh '{}': needs {} vertex+index bytes, has {}", meshVfsPath, vertexBytes + indexBytes, reader.Remaining());
				AE_UNEXPECTED(AetherError::Asset("truncated mesh data: " + std::string(meshVfsPath)));
			}
			if (!budget.Add(data.size(), 32) ||
			    !budget.Add(meshVfsPath.size(), static_cast<std::size_t>(hdr.materialCount) * 16 + 16))
				AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));

			AE_VERBOSE(LogCategory::Engine, "Loading mesh '{}': {} verts, {} indices, {} materials, skin={}", meshVfsPath, hdr.vertexCount, hdr.indexCount, hdr.materialCount, hdr.skinRefPathLen > 0 ? "yes" : "no");
			AE_VERBOSE(LogCategory::Engine, "  AABB: [{}, {}, {}] -> [{}, {}, {}]", hdr.aabbMin[0], hdr.aabbMin[1], hdr.aabbMin[2], hdr.aabbMax[0], hdr.aabbMax[1], hdr.aabbMax[2]);
			AE_VERBOSE(LogCategory::Engine, "  Sphere: center=[{}, {}, {}], radius={}", hdr.sphereCenter[0], hdr.sphereCenter[1], hdr.sphereCenter[2], hdr.sphereRadius);
			AE_VERBOSE(LogCategory::Engine, "  Index type: {}", hdr.indexType == 0 ? "uint16" : "uint32");

			GltfAsset asset;

			std::vector<DiskMeshVertex> diskVerts(hdr.vertexCount);
			reader.ReadRaw(diskVerts.data(), hdr.vertexCount * sizeof(DiskMeshVertex));

			std::vector<uint32_t> indices(hdr.indexCount);
			if (hdr.indexType == 0)
			{
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

			// Every index must reference a vertex that exists; the submesh loops below copy
			// vertices straight through diskVerts[idx], so a single bad index walks off the
			// end of the vertex blob.
			for (uint32_t i = 0; i < hdr.indexCount; ++i)
			{
				if (indices[i] >= hdr.vertexCount)
				{
					AE_WARN(LogCategory::Engine, "Mesh index {} -> vertex {} out of range ({} verts) in '{}'", i, indices[i], hdr.vertexCount, meshVfsPath);
					AE_UNEXPECTED(AetherError::Asset("mesh index out of range: " + std::string(meshVfsPath)));
				}
			}

			if (static_cast<std::uint64_t>(reader.Remaining()) < static_cast<std::uint64_t>(hdr.subMeshCount) * sizeof(SubMeshHeaderDisk))
				AE_UNEXPECTED(AetherError::Asset("truncated mesh subheaders: " + std::string(meshVfsPath)));
			std::vector<SubMeshHeaderDisk> subMeshes(hdr.subMeshCount);
			for (uint32_t sm = 0; sm < hdr.subMeshCount; ++sm)
			{
				subMeshes[sm] = reader.Read<SubMeshHeaderDisk>();
			}
			std::vector<std::string> primitiveKeys(hdr.subMeshCount);
			if (hdr.version >= 4)
			{
				for (auto& key : primitiveKeys)
				{
					std::uint16_t length = 0;
					if (!reader.TryRead(length) || !reader.CanRead(length))
						AE_UNEXPECTED(AetherError::Asset("truncated mesh primitive key: " + std::string(meshVfsPath)));
					key.resize(length);
					if (length != 0)
						reader.ReadRaw(key.data(), length);
					if (key.empty())
						key = "unidentified";
				}
			}
			for (uint32_t sm = 0; sm < hdr.subMeshCount; ++sm)
			{
				const auto& rangeHdr = subMeshes[sm];
				// Subtraction form: firstIndex is checked against the size before the
				// difference is taken, so the addition can never overflow.
				if (rangeHdr.firstIndex > indices.size() || rangeHdr.indexCount > indices.size() - rangeHdr.firstIndex)
				{
					AE_WARN(LogCategory::Engine, "Submesh {} index range [{}..{}] out of range ({} indices) in '{}'", sm, rangeHdr.firstIndex, rangeHdr.firstIndex + rangeHdr.indexCount, indices.size(), meshVfsPath);
					AE_UNEXPECTED(AetherError::Asset("mesh submesh index range out of range: " + std::string(meshVfsPath)));
				}
			}

			std::string skinRefPath;
			if (hdr.skinRefPathLen > 0)
			{
				skinRefPath.resize(hdr.skinRefPathLen);
				reader.ReadRaw(skinRefPath.data(), hdr.skinRefPathLen);
				AE_VERBOSE(LogCategory::Engine, "  Skin ref: {}", skinRefPath);
			}

			std::vector<std::string> matPaths(hdr.materialCount);
			for (uint32_t i = 0; i < hdr.materialCount; ++i)
			{
				matPaths[i] = reader.ReadString();
				AE_VERBOSE(LogCategory::Engine, "  Material[{}]: {}", i, matPaths[i]);
			}

			auto ConvertVertex = [](const DiskMeshVertex& src, MeshVertex& dst)
			{
				dst.position = glm::vec3(src.position[0], src.position[1], src.position[2]);
				dst.normal = glm::vec3(src.normal[0], src.normal[1], src.normal[2]);
				dst.tangent = glm::vec4(src.tangent[0], src.tangent[1], src.tangent[2], src.tangent[3]);
				dst.uv = glm::vec2(src.uv[0], src.uv[1]);
				dst.uv2 = glm::vec2(src.uv2[0], src.uv2[1]);
				dst.color = UnpackColorRGBA8(src.color);
				dst.jointIndices = glm::uvec4(src.jointIndices[0], src.jointIndices[1], src.jointIndices[2], src.jointIndices[3]);
				dst.jointWeights = glm::vec4(src.jointWeights[0], src.jointWeights[1], src.jointWeights[2], src.jointWeights[3]);
			};

			const int32_t skinIndex = !skinRefPath.empty() ? 0 : -1;
			if (hdr.subMeshCount > 0)
			{
				for (uint32_t sm = 0; sm < hdr.subMeshCount; ++sm)
				{
					const auto& smHdr = subMeshes[sm];
					GltfPrimitive prim;
					prim.nodeIndex = 0;
					prim.materialIndex = (smHdr.materialIndex < hdr.materialCount) ? static_cast<int32_t>(smHdr.materialIndex) : static_cast<int32_t>(hdr.materialCount > 0 ? 0 : -1);
					prim.skinIndex = skinIndex;
					if (hdr.version >= 4)
						prim.primitiveKey = primitiveKeys[sm];

					uint32_t minVert = UINT32_MAX;
					uint32_t maxVert = 0;
					for (uint32_t k = 0; k < smHdr.indexCount; ++k)
					{
						const uint32_t idx = indices[smHdr.firstIndex + k];
						minVert = std::min(minVert, idx);
						maxVert = std::max(maxVert, idx);
					}
					if (smHdr.indexCount == 0)
						AE_UNEXPECTED(AetherError::Asset("empty mesh submesh: " + std::string(meshVfsPath)));
					const std::size_t range = static_cast<std::size_t>(maxVert) - minVert + 1;
					if (!budget.Add(range, sizeof(uint32_t)) ||
					    !budget.Add(smHdr.indexCount, sizeof(uint32_t) + sizeof(MeshVertex)) ||
					    !budget.Add(sizeof(GltfPrimitive) * 2))
						AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));

					const uint32_t vertRange = maxVert - minVert + 1;
					std::vector<uint32_t> remap(vertRange, UINT32_MAX);
					uint32_t localVerts = 0;
					prim.indices.resize(smHdr.indexCount);
					for (uint32_t k = 0; k < smHdr.indexCount; ++k)
					{
						const uint32_t oldIdx = indices[smHdr.firstIndex + k];
						const uint32_t off = oldIdx - minVert;
						if (remap[off] == UINT32_MAX)
						{
							remap[off] = localVerts++;
						}
						prim.indices[k] = remap[off];
					}

					prim.vertices.resize(localVerts);
					for (uint32_t v = minVert; v <= maxVert; ++v)
					{
						const uint32_t off = v - minVert;
						if (remap[off] != UINT32_MAX)
						{
							ConvertVertex(diskVerts[v], prim.vertices[remap[off]]);
						}
					}

					std::memcpy(prim.aabbMin, hdr.aabbMin, sizeof(prim.aabbMin));
					std::memcpy(prim.aabbMax, hdr.aabbMax, sizeof(prim.aabbMax));
					std::memcpy(prim.sphereCenter, hdr.sphereCenter, sizeof(prim.sphereCenter));
					prim.sphereRadius = hdr.sphereRadius;

					asset.primitives.push_back(std::move(prim));
				}
			}
			else
			{
				GltfPrimitive prim;
				if (hdr.version >= 4)
					prim.primitiveKey = "unidentified";
				prim.nodeIndex = 0;
				prim.materialIndex = hdr.materialCount > 0 ? 0 : -1;
				prim.skinIndex = skinIndex;
				prim.vertices.resize(hdr.vertexCount);
				prim.indices = std::move(indices);

				std::memcpy(prim.aabbMin, hdr.aabbMin, sizeof(prim.aabbMin));
				std::memcpy(prim.aabbMax, hdr.aabbMax, sizeof(prim.aabbMax));
				std::memcpy(prim.sphereCenter, hdr.sphereCenter, sizeof(prim.sphereCenter));
				prim.sphereRadius = hdr.sphereRadius;

				for (uint32_t v = 0; v < hdr.vertexCount; ++v)
				{
					ConvertVertex(diskVerts[v], prim.vertices[v]);
				}

				asset.primitives.push_back(std::move(prim));
			}

			GltfNode rootNode;
			rootNode.name = "root";
			rootNode.meshIndex = 0;
			rootNode.skinIndex = skinIndex;
			rootNode.parentIndex = -1;
			asset.nodes.push_back(std::move(rootNode));

			if (!skinRefPath.empty())
			{
				const std::string skelPath = ResolveSkelPath(meshVfsPath, skinRefPath);
				if (!skelPath.empty())
				{
					GltfSkin skin;
					if (LoadSkeleton(skelPath, asset, skin, budget))
					{
						asset.skins.push_back(std::move(skin));
					}
				}
				else
				{
					AE_WARN(LogCategory::Engine, "Skeleton file not resolved for skin ref: {}", skinRefPath);
				}
				if (budget.rejected)
					AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));
			}

			for (uint32_t i = 0; i < hdr.materialCount; ++i)
			{
				GltfMaterial mat;
				mat.name = "material_" + std::to_string(i);

				const std::string matFullPath = ResolveRelativeVfsPath(meshVfsPath, matPaths[i]);

				if (!LoadMaterialBinary(matFullPath, mat, budget))
				{
					const std::string tomlPath = matFullPath + "/properties.toml";
					if (io::FileSystem::Exists(tomlPath))
					{
						mat.name = matPaths[i];
					}
				}
				else
				{
					mat.name = matPaths[i];
					AE_VERBOSE(LogCategory::Engine, "  Material '{}' loaded (albedo={}, normal={}, orm={})", mat.name, mat.albedoPath.empty() ? "no" : "yes", mat.normalPath.empty() ? "no" : "yes", mat.metallicRoughnessPath.empty() ? "no" : "yes");
				}
				if (budget.rejected)
					AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));

				asset.materials.push_back(std::move(mat));
			}

			std::string animSetPath = DeriveAnimSetPath(meshVfsPath);
			if (!animSetPath.empty())
			{
				AE_VERBOSE(LogCategory::Engine, "  AnimSet: {}", animSetPath);
				auto animSetData = budget.Read(animSetPath);
				if (budget.rejected)
					AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));
				if (animSetData.has_value())
				{
					BinaryReader animSetReader(*animSetData);
					AnimSetHeaderDisk asetHdr;
					// TryRead, not Read: see LoadFromMesh's own comment on why -
					// AnimSetHeaderDisk also default-initialises magic/version to its
					// own valid values.
					if (animSetReader.TryRead(asetHdr) && CheckMagic(asetHdr) && asetHdr.version == ASET_VERSION)
					{
						AE_VERBOSE(LogCategory::Engine, "  AnimSet: {} animations, skeletonHash={}", asetHdr.animCount, asetHdr.skeletonHash);

						const std::string mountRoot = animSetPath.substr(0, animSetPath.find("://") + 3);

						const uint32_t boneNodeOffset = (!asset.skins.empty() && !asset.skins[0].joints.empty()) ? static_cast<uint32_t>(asset.nodes.size() - asset.skins[0].joints.size()) : 0u;
						AE_VERBOSE(LogCategory::Engine, "  AnimSet: boneNodeOffset={}, asset.nodes.size={}, skin joints={}", boneNodeOffset, asset.nodes.size(), asset.skins.empty() ? 0 : asset.skins[0].joints.size());
						// Each entry is at least a length-prefixed string (2 bytes); without
						// this bound a corrupt animCount spins the loop billions of times.
						if (static_cast<uint64_t>(asetHdr.animCount) * 2 > animSetReader.Remaining())
						{
							AE_WARN(LogCategory::Engine, "AnimSet animation count {} out of range ({} bytes left): {}", asetHdr.animCount, animSetReader.Remaining(), animSetPath);
						}
						else
						{
							if (!budget.Add(mountRoot.size(), static_cast<std::size_t>(asetHdr.animCount)) ||
							    !budget.Add(asetHdr.animCount, sizeof(GltfAnimation) * 2))
								AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));
							// A skinless-but-animated model (no <skin> at all, so
							// boneNodeOffset above is always 0) was baked with each
							// channel's target NODE NAME instead of a real skeleton
							// index (MeshProcessor's ProcessAnimations, nameBased path) -
							// there is no skeleton to offset into. Resolve those channels
							// against this asset's own already-loaded node list, once,
							// right here, so everything downstream (AnimationDatabase::
							// Create) sees a real nodeIndex exactly like a skinned model
							// would. Keep the clip's own name unlike
							// RemapAnimationByBoneName (used for cross-skeleton ragdoll
							// retargeting elsewhere in this file, where a "(remapped)"
							// suffix is the point) - scripts identify a clip by
							// Animation.Find(name), and this is the SAME model's own
							// animation, not a borrowed one.
							std::unordered_map<std::string, uint32_t> ownNodeNames;
							for (uint32_t ni = 0; ni < asset.nodes.size(); ++ni)
							{
								if (!asset.nodes[ni].name.empty())
								{
									ownNodeNames.emplace(asset.nodes[ni].name, ni);
								}
							}

							for (uint32_t i = 0; i < asetHdr.animCount; ++i)
							{
								const std::string animRelPath = animSetReader.ReadString();
								std::string animFullPath = mountRoot + animRelPath;
								if (io::FileSystem::Exists(animFullPath))
								{
									GltfAnimation anim = LoadAnimation(animFullPath, budget);
									if (budget.rejected)
										AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));
									if (!anim.name.empty())
									{
										if (boneNodeOffset > 0)
										{
											AE_VERBOSE(LogCategory::Engine, "    Offsetting {} channels by +{}", anim.channels.size(), boneNodeOffset);
											for (auto& ch: anim.channels)
											{
												ch.nodeIndex += boneNodeOffset;
											}
										}
										uint32_t unresolvedByName = 0;
										for (auto& ch: anim.channels)
										{
											if (ch.boneName.empty())
											{
												continue;
											}
											if (const auto it = ownNodeNames.find(ch.boneName); it != ownNodeNames.end())
											{
												ch.nodeIndex = it->second;
											}
											else
											{
												++unresolvedByName;
											}
											ch.boneName.clear();
										}
										if (unresolvedByName > 0)
										{
											AE_WARN(LogCategory::Engine, "  Animation '{}': {} channel(s) named a node this model does not have; those channels keep whatever index was baked (likely wrong)", anim.name, unresolvedByName);
										}
										asset.animations.push_back(std::move(anim));
									}
								}
								else
								{
									AE_WARN(LogCategory::Engine, "  Animation file not found: {}", animFullPath);
								}
							}
						}
					}
					else
					{
						AE_WARN(LogCategory::Engine, "Invalid animset magic: {}", animSetPath);
					}
				}
			}

			AE_VERBOSE(LogCategory::Engine, "Mesh loaded: {} nodes, {} skins, {} materials, {} animations", asset.nodes.size(), asset.skins.size(), asset.materials.size(), asset.animations.size());

			return asset;
		}
	} // namespace

	std::string GltfAsset::PrimitiveKey(const GltfPrimitive& primitive)
	{
		if (!primitive.primitiveKey.empty())
			return primitive.primitiveKey;
		return GeometryPrimitiveKey(primitive);
	}

	std::string GltfAsset::GeometryPrimitiveKey(const GltfPrimitive& primitive)
	{
		std::uint64_t a = 14695981039346656037ull;
		std::uint64_t b = 7809847782465536322ull;
		auto add = [&](const void* data, std::size_t size)
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (std::size_t i = 0; i < size; ++i)
			{
				a = (a ^ bytes[i]) * 1099511628211ull;
				b = (b ^ bytes[i]) * 14029467366897019727ull;
			}
		};
		const std::uint64_t vertexCount = primitive.vertices.size();
		const std::uint64_t indexCount = primitive.indices.size();
		add(&vertexCount, sizeof(vertexCount));
		add(&indexCount, sizeof(indexCount));
		for (const auto& vertex : primitive.vertices)
			add(&vertex, sizeof(vertex));
		for (const auto index : primitive.indices)
			add(&index, sizeof(index));
		constexpr char hex[] = "0123456789abcdef";
		std::string key = "geometry:";
		for (const auto value : {a, b})
			for (int shift = 60; shift >= 0; shift -= 4)
				key += hex[(value >> shift) & 15];
		return key;
	}

	Expected<GltfAsset> GltfAsset::LoadFromVfsPath(std::string_view path)
	{
		AE_PROFILE_ZONE();
		AE_PROFILE_SET_ZONE_NAME(path.data());

		const std::string vfsPath = io::FileSystem::ResolveAssetPath(path);
		if (vfsPath.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Unknown asset UID: " + std::string(path)));
		}

		auto TryMesh = [&](const std::string& meshPath) -> bool
		{
			return !meshPath.empty() && io::FileSystem::Exists(meshPath);
		};

		// magic"). In the pak-mounted runtime the raw source never exists, so this
		std::string meshPath = vfsPath;
		if (!vfsPath.ends_with(".mesh"))
		{
			const std::string derived = GltfAsset::ResolveMeshPath(vfsPath);
			if (TryMesh(derived))
			{
				meshPath = derived;
			}
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
			AE_PROFILE_ZONE();
			AE_TRY(data, io::FileSystem::ReadFile(meshPath));
			meshData = std::move(*data);
		}
		ParseBudget budget;
		return LoadFromMesh(meshData, meshPath, budget);
	}

	Expected<GltfAsset> GltfAsset::LoadFromMemory(const std::vector<std::byte>& meshData, std::string_view debugPath)
	{
		const std::string resolved = io::FileSystem::ResolveAssetPath(debugPath);
		if (resolved.empty() && !debugPath.empty())
		{
			AE_UNEXPECTED(AetherError::Asset("Unknown asset UID: " + std::string(debugPath)));
		}
		ParseBudget budget;
		return LoadFromMesh(meshData, resolved, budget);
	}

	Expected<GltfAsset> GltfAsset::LoadFromMemory(const std::vector<std::byte>& meshData, std::string_view debugPath, const std::function<bool(std::size_t)>& reserve)
	{
		ParseBudget budget{&reserve};
		if (!budget.Add(debugPath.size(), 8))
			AE_UNEXPECTED(AetherError::Asset("Streaming decode reservation rejected"));
		const std::string resolved = io::FileSystem::ResolveAssetPath(debugPath);
		if (resolved.empty() && !debugPath.empty())
			AE_UNEXPECTED(AetherError::Asset("Unknown asset UID: " + std::string(debugPath)));
		return LoadFromMesh(meshData, resolved, budget);
	}

	std::string GltfAsset::ResolveMeshPath(std::string_view vfsPath)
	{
		const std::string resolved = io::FileSystem::ResolveAssetPath(vfsPath);
		vfsPath = resolved;
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
