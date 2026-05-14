#include "assets/GltfAsset.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <AeBnFormat.hpp>

#include "FileSystem.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

namespace aether::assets
{
	namespace
	{
		std::pair<std::string_view, std::string_view> SplitVfsPath(std::string_view vfsPath)
		{
			constexpr std::string_view kSeparator = "://";
			const std::size_t sep = vfsPath.find(kSeparator);
			if (sep == std::string_view::npos)
			{
				AE_ASSERT_ALWAYS(false, "Invalid VFS path (missing ://): " + std::string(vfsPath));
			}
			return { vfsPath.substr(0, sep), vfsPath.substr(sep + kSeparator.size()) };
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
		// "mount://path/model.gltf" -> "mount://path/model.mesh"
		// This is the public ResolveMeshPath implementation.
		std::string DeriveAebnPath(std::string_view vfsPath)
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

		// -------------------------------------------------------------------------
		// AEBN binary deserialiser
		// -------------------------------------------------------------------------

		Expected<GltfAsset> LoadFromAebn(const std::vector<std::byte>& data, std::string_view meshVfsPath)
		{
			const std::byte* p = data.data();
			const std::byte* end = data.data() + data.size();

			auto CheckSpace = [&](std::size_t n) -> bool
			{
				if (static_cast<std::size_t>(end - p) < n)
				{
					return false;
				}
				return true;
			};

			auto ReadT = [&]<typename T>() -> T
			{
				if (!CheckSpace(sizeof(T)))
				{
					return T{};
				}
				T val;
				std::memcpy(&val, p, sizeof(T));
				p += sizeof(T);
				return val;
			};

			auto ReadStr = [&](std::uint16_t len) -> std::string
			{
				if (!CheckSpace(len))
				{
					return {};
				}
				std::string s(reinterpret_cast<const char*>(p), len);
				p += len;
				return s;
			};

			const AeBnHeader hdr = ReadT.template operator()<AeBnHeader>();
			if (std::memcmp(hdr.magic, AEBN_MAGIC, 4) != 0 || hdr.version != AEBN_VERSION)
			{
				AE_UNEXPECTED(AetherError::Asset("invalid magic or version: " + std::string(meshVfsPath)));
			}

			GltfAsset asset;

			// Images
			asset.images.reserve(hdr.imageCount);
			for (std::uint32_t i = 0; i < hdr.imageCount; ++i)
			{
				const auto ih = ReadT.template operator()<AeBnImageHeader>();
				std::string name = ReadStr(ih.nameLen);
				const std::string relUri = ReadStr(ih.uriLen);
				asset.images.push_back({
				        .name = std::move(name),
				        .uri = relUri.empty() ? std::string() : ResolveRelativeVfsPath(meshVfsPath, relUri),
				});
			}

			// Textures
			asset.textures.reserve(hdr.textureCount);
			for (std::uint32_t i = 0; i < hdr.textureCount; ++i)
			{
				const auto th = ReadT.template operator()<AeBnTextureHeader>();
				asset.textures.push_back({
				        .name = ReadStr(th.nameLen),
				        .imageIndex = th.imageIndex,
				});
			}

			// Materials
			asset.materials.reserve(hdr.materialCount);
			for (std::uint32_t i = 0; i < hdr.materialCount; ++i)
			{
				const auto mh = ReadT.template operator()<AeBnMaterialHeader>();
				GltfMaterial mat;
				mat.name = ReadStr(mh.nameLen);
				mat.baseColorFactor = glm::vec4(mh.baseColorFactor[0], mh.baseColorFactor[1], mh.baseColorFactor[2], mh.baseColorFactor[3]);
				mat.metallicFactor = mh.metallicFactor;
				mat.roughnessFactor = mh.roughnessFactor;
				mat.emissiveFactor = glm::vec3(mh.emissiveFactor[0], mh.emissiveFactor[1], mh.emissiveFactor[2]);
				mat.alphaCutoff = mh.alphaCutoff;
				mat.baseColorTexture = mh.baseColorTexture;
				mat.metallicRoughnessTexture = mh.metallicRoughnessTexture;
				mat.normalTexture = mh.normalTexture;
				mat.occlusionTexture = mh.occlusionTexture;
				mat.emissiveTexture = mh.emissiveTexture;
				mat.doubleSided = mh.doubleSided != 0;
				mat.alphaBlend = mh.alphaBlend != 0;
				mat.alphaMask = mh.alphaMask != 0;
				asset.materials.push_back(std::move(mat));
			}

			// Nodes
			asset.nodes.resize(hdr.nodeCount);
			for (std::uint32_t i = 0; i < hdr.nodeCount; ++i)
			{
				const auto nh = ReadT.template operator()<AeBnNodeHeader>();
				GltfNode& node = asset.nodes[i];
				node.parentIndex = nh.parentIndex;
				node.meshIndex = nh.meshIndex;
				node.skinIndex = nh.skinIndex;
				node.translation = glm::vec3(nh.translation[0], nh.translation[1], nh.translation[2]);
				// AEBN stores xyzw; GLM quat ctor is (w,x,y,z)
				node.rotation = glm::quat(nh.rotation[3], nh.rotation[0], nh.rotation[1], nh.rotation[2]);
				node.scale = glm::vec3(nh.scale[0], nh.scale[1], nh.scale[2]);
				node.hasMatrix = nh.hasMatrix != 0;
				if (node.hasMatrix)
				{
					std::memcpy(&node.matrix[0][0], nh.matrix, sizeof(nh.matrix));
				}
				node.children.resize(nh.childCount);
				for (std::uint32_t c = 0; c < nh.childCount; ++c)
				{
					node.children[c] = ReadT.template operator()<std::uint32_t>();
				}
				node.name = ReadStr(nh.nameLen);
			}

			// Skins
			asset.skins.reserve(hdr.skinCount);
			for (std::uint32_t i = 0; i < hdr.skinCount; ++i)
			{
				const auto sh = ReadT.template operator()<AeBnSkinHeader>();
				GltfSkin skin;
				skin.skeletonRoot = sh.skeletonRoot;
				skin.name = ReadStr(sh.nameLen);
				skin.joints.resize(sh.jointCount);
				for (std::uint32_t j = 0; j < sh.jointCount; ++j)
				{
					skin.joints[j] = ReadT.template operator()<std::uint32_t>();
				}
				skin.inverseBindMatrices.resize(sh.jointCount);
				for (std::uint32_t j = 0; j < sh.jointCount; ++j)
				{
					if (!CheckSpace(64))
					{
						AE_UNEXPECTED(AetherError::Asset("truncated data in " + std::string(meshVfsPath)));
					}
					std::memcpy(&skin.inverseBindMatrices[j][0][0], p, 64);
					p += 64;
				}
				asset.skins.push_back(std::move(skin));
			}

			// Primitives
			asset.primitives.reserve(hdr.primitiveCount);
			for (std::uint32_t i = 0; i < hdr.primitiveCount; ++i)
			{
				const auto ph = ReadT.template operator()<AeBnPrimitiveHeader>();
				GltfPrimitive prim;
				prim.nodeIndex = ph.nodeIndex;
				prim.materialIndex = ph.materialIndex;
				prim.skinIndex = ph.skinIndex;
				prim.vertices.resize(ph.vertexCount);
				if (!CheckSpace(ph.vertexCount * sizeof(AeBnVertex)))
				{
					AE_UNEXPECTED(AetherError::Asset("truncated data in " + std::string(meshVfsPath)));
				}
				for (std::uint32_t v = 0; v < ph.vertexCount; ++v)
				{
					AeBnVertex src;
					std::memcpy(&src, p, sizeof(AeBnVertex));
					p += sizeof(AeBnVertex);
					Mesh::Vertex& dst = prim.vertices[v];
					dst.position = glm::vec3(src.position[0], src.position[1], src.position[2]);
					dst.normal = glm::vec3(src.normal[0], src.normal[1], src.normal[2]);
					dst.tangent = glm::vec4(src.tangent[0], src.tangent[1], src.tangent[2], src.tangent[3]);
					dst.uv = glm::vec2(src.uv[0], src.uv[1]);
					dst.color = glm::vec3(src.color[0], src.color[1], src.color[2]);
					dst.jointIndices = glm::uvec4(src.jointIndices[0], src.jointIndices[1], src.jointIndices[2], src.jointIndices[3]);
					dst.jointWeights = glm::vec4(src.jointWeights[0], src.jointWeights[1], src.jointWeights[2], src.jointWeights[3]);
				}
				prim.indices.resize(ph.indexCount);
				if (!CheckSpace(ph.indexCount * sizeof(std::uint32_t)))
				{
					AE_UNEXPECTED(AetherError::Asset("truncated data in " + std::string(meshVfsPath)));
				}
				std::memcpy(prim.indices.data(), p, ph.indexCount * sizeof(std::uint32_t));
				p += ph.indexCount * sizeof(std::uint32_t);
				asset.primitives.push_back(std::move(prim));
			}

			// Animations
			asset.animations.reserve(hdr.animCount);
			for (std::uint32_t ai = 0; ai < hdr.animCount; ++ai)
			{
				const auto ah = ReadT.template operator()<AeBnAnimHeader>();
				GltfAnimation anim;
				anim.name = ReadStr(ah.nameLen);
				anim.channels.reserve(ah.channelCount);
				for (std::uint32_t ci = 0; ci < ah.channelCount; ++ci)
				{
					const auto ch = ReadT.template operator()<AeBnChannelHeader>();
					GltfAnimationChannel channel;
					channel.nodeIndex = ch.nodeIndex;
					switch (static_cast<AeBnAnimPath>(ch.path))
					{
						case AeBnAnimPath::Translation:
							channel.path = GltfAnimationPath::Translation;
							break;
						case AeBnAnimPath::Rotation:
							channel.path = GltfAnimationPath::Rotation;
							break;
						case AeBnAnimPath::Scale:
							channel.path = GltfAnimationPath::Scale;
							break;
						case AeBnAnimPath::Weights:
							channel.path = GltfAnimationPath::Weights;
							break;
					}
					switch (static_cast<AeBnInterp>(ch.interp))
					{
						case AeBnInterp::Linear:
							channel.interpolation = GltfInterpolation::Linear;
							break;
						case AeBnInterp::Step:
							channel.interpolation = GltfInterpolation::Step;
							break;
						case AeBnInterp::CubicSpline:
							channel.interpolation = GltfInterpolation::CubicSpline;
							break;
					}
					channel.times.resize(ch.keyCount);
					if (!CheckSpace(ch.keyCount * sizeof(float)))
					{
						AE_UNEXPECTED(AetherError::Asset("truncated data in " + std::string(meshVfsPath)));
					}
					std::memcpy(channel.times.data(), p, ch.keyCount * sizeof(float));
					p += ch.keyCount * sizeof(float);
					channel.values.resize(ch.keyCount);
					if (!CheckSpace(ch.keyCount * 4 * sizeof(float)))
					{
						AE_UNEXPECTED(AetherError::Asset("truncated data in " + std::string(meshVfsPath)));
					}
					for (std::uint32_t k = 0; k < ch.keyCount; ++k)
					{
						float v4[4];
						std::memcpy(v4, p, sizeof(v4));
						p += sizeof(v4);
						channel.values[k] = glm::vec4(v4[0], v4[1], v4[2], v4[3]);
					}
					anim.channels.push_back(std::move(channel));
				}
				asset.animations.push_back(std::move(anim));
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
		auto TryAebn = [&](const std::string& meshPath) -> bool
		{
			return !meshPath.empty() && io::FileSystem::Exists(meshPath);
		};

		// Accept .mesh directly, or derive from .gltf / .glb / any extension.
		std::string meshPath = vfsPath;
		if (!TryAebn(meshPath))
		{
			meshPath = DeriveAebnPath(vfsPath);
		}

		if (!TryAebn(meshPath))
		{
			AE_UNEXPECTED(AetherError::Asset("packed .mesh not found for '" + vfsPath + "'. Run AssetPacker to generate it."));
		}

		std::vector<std::byte> meshData;
		{
			AE_PROFILE_ZONE_N("GltfAsset::LoadAebn");
			AE_EXPECT_OR_THROW(data, io::FileSystem::ReadFile(meshPath));
			meshData = std::move(data);

			if (meshData.size() >= sizeof(AeBnHeader))
			{
				AeBnHeader hdr{};
				std::memcpy(&hdr, meshData.data(), sizeof(hdr));
				if (std::memcmp(hdr.magic, AEBN_MAGIC, 4) == 0 && hdr.version != AEBN_VERSION)
				{
					AE_UNEXPECTED(AetherError::Asset("stale .mesh cache (version " + std::to_string(hdr.version) + ", expected " + std::to_string(AEBN_VERSION) + ") for '" + meshPath + "'. Re-run AssetPacker."));
				}
			}
		}
		return LoadFromAebn(meshData, meshPath);
	}

	Expected<GltfAsset> GltfAsset::LoadFromMemory(std::vector<std::byte> meshData, std::string_view debugPath)
	{
		if (meshData.size() >= sizeof(AeBnHeader))
		{
			AeBnHeader hdr{};
			std::memcpy(&hdr, meshData.data(), sizeof(hdr));
			if (std::memcmp(hdr.magic, AEBN_MAGIC, 4) == 0 && hdr.version != AEBN_VERSION)
			{
				AE_UNEXPECTED(AetherError::Asset("stale .mesh cache (version " + std::to_string(hdr.version) + ", expected " + std::to_string(AEBN_VERSION) + ") for '" + std::string(debugPath) + "'. Re-run AssetPacker."));
			}
		}
		return LoadFromAebn(meshData, debugPath);
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
