#include "assets/AssetManager.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include <BinaryFormats.hpp>

#include "assets/GltfAsset.hpp"
#include "gpu/BindlessManager.hpp"
#include "scene/EcsHelpers.hpp"
#include "io/FileSystem.hpp"
#include "io/FileGlobOptions.hpp"
#include "utils/BinaryReader.hpp"
#include "utils/Logger.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/ShadowService.hpp"
#include "utils/TextIni.hpp"
#include "vulkan/VulkanContext.hpp"
#include "scene/World.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		struct MaterialPresetSpec
		{
			MaterialAsset material{};
			std::string albedoPath;
			std::string normalPath;
			std::string metallicRoughnessPath; // glTF ORM: G=roughness, B=metallic
			std::string occlusionPath;
			std::string emissivePath;
		};

		std::string NormalizeVirtualFolder(std::string path)
		{
			while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
			{
				path.pop_back();
			}
			return path;
		}

		std::string ResolvePathRelativeTo(std::string_view basePath, const std::string& resourcePath)
		{
			if (resourcePath.empty())
			{
				return {};
			}

			if (resourcePath.find("://") != std::string::npos)
			{
				return resourcePath;
			}

			const std::string base(basePath);
			const std::size_t mountPos = base.find("://");
			if (mountPos == std::string::npos)
			{
				return resourcePath;
			}

			const std::string mount = base.substr(0, mountPos);
			const std::filesystem::path rel = base.substr(mountPos + 3);
			const std::filesystem::path dir = rel.parent_path();
			const std::filesystem::path resolved = (dir / resourcePath).lexically_normal();
			return mount + "://" + resolved.generic_string();
		}

		std::string ResolvePathInFolder(std::string_view folderPath, const std::string& resourcePath)
		{
			if (resourcePath.empty())
			{
				return {};
			}
			if (resourcePath.find("://") != std::string::npos)
			{
				return resourcePath;
			}
			const std::string folder = NormalizeVirtualFolder(std::string(folderPath));
			return folder + "/" + resourcePath;
		}

		std::string ResolveStemInFolder(std::string_view folderPath, const std::string& stem)
		{
			if (stem.empty())
			{
				return {};
			}

			if (stem.find("://") != std::string::npos)
			{
				return io::FileSystem::Exists(stem) ? stem : std::string();
			}

			const std::string direct = ResolvePathInFolder(folderPath, stem);
			if (io::FileSystem::Exists(direct))
			{
				return direct;
			}

			const std::string noExt = std::filesystem::path(stem).extension().empty() ? stem : std::filesystem::path(stem).stem().string();
			constexpr std::string_view kExts[] = {".texture", ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".webp", ".dds", ".ktx2"};
			for (const std::string_view ext: kExts)
			{
				const std::string candidate = ResolvePathInFolder(folderPath, noExt + std::string(ext));
				if (io::FileSystem::Exists(candidate))
				{
					return candidate;
				}
			}

			return {};
		}

		std::string ResolveFirstAliasInFolder(std::string_view folderPath, std::initializer_list<std::string_view> aliases)
		{
			for (const std::string_view alias: aliases)
			{
				const std::string resolved = ResolveStemInFolder(folderPath, std::string(alias));
				if (!resolved.empty())
				{
					return resolved;
				}
			}
			return {};
		}

		std::string ResolvePresetPath(std::string_view presetPath, const std::string& texturePath)
		{
			return ResolvePathRelativeTo(presetPath, texturePath);
		}

		MaterialPresetSpec ParseMaterialPreset(std::string_view path, const std::string& text)
		{
			MaterialPresetSpec spec;

			text::ParseToml(text,
			        [&spec, path](const text::IniEntry& entry)
			        {
				        if (entry.fullKey == "material.basecolorfactor" || entry.fullKey == "basecolorfactor")
				        {
					        if (const auto parsed = text::ParseFloatArray<4>(entry.value))
					        {
						        spec.material.baseColorFactor = glm::vec4((*parsed)[0], (*parsed)[1], (*parsed)[2], (*parsed)[3]);
					        }
					        return;
				        }
				        if (entry.fullKey == "material.emissivefactor" || entry.fullKey == "emissivefactor")
				        {
					        if (const auto parsed = text::ParseFloatArray<3>(entry.value))
					        {
						        spec.material.emissiveFactor = glm::vec3((*parsed)[0], (*parsed)[1], (*parsed)[2]);
					        }
					        return;
				        }
				        if (entry.fullKey == "material.metallicfactor" || entry.fullKey == "metallicfactor")
				        {
					        if (const auto parsed = text::ParseFloat(entry.value))
					        {
						        spec.material.metallicFactor = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "material.roughnessfactor" || entry.fullKey == "roughnessfactor")
				        {
					        if (const auto parsed = text::ParseFloat(entry.value))
					        {
						        spec.material.roughnessFactor = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "material.occlusionstrength" || entry.fullKey == "occlusionstrength")
				        {
					        if (const auto parsed = text::ParseFloat(entry.value))
					        {
						        spec.material.occlusionStrength = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "material.alphacutoff" || entry.fullKey == "alphacutoff")
				        {
					        if (const auto parsed = text::ParseFloat(entry.value))
					        {
						        spec.material.alphaCutoff = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "material.doublesided" || entry.fullKey == "doublesided")
				        {
					        if (const auto parsed = text::ParseBool(entry.value))
					        {
						        spec.material.doubleSided = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "material.alphablend" || entry.fullKey == "alphablend")
				        {
					        if (const auto parsed = text::ParseBool(entry.value))
					        {
						        spec.material.alphaBlend = *parsed;
					        }
					        return;
				        }
				        if (entry.fullKey == "material.alphamask" || entry.fullKey == "alphamask")
				        {
					        if (const auto parsed = text::ParseBool(entry.value))
					        {
						        spec.material.alphaMask = *parsed;
					        }
					        return;
				        }

				        if (entry.fullKey == "textures.albedo" || entry.fullKey == "albedo" || entry.fullKey == "textures.basecolor")
				        {
					        spec.albedoPath = ResolvePresetPath(path, entry.value);
					        return;
				        }
				        if (entry.fullKey == "textures.normal" || entry.fullKey == "normal")
				        {
					        spec.normalPath = ResolvePresetPath(path, entry.value);
					        return;
				        }
				        if (entry.fullKey == "textures.metallicroughness" || entry.fullKey == "metallicroughness")
				        {
					        spec.metallicRoughnessPath = ResolvePresetPath(path, entry.value);
					        return;
				        }

				        if (entry.fullKey == "textures.occlusion" || entry.fullKey == "occlusion" || entry.fullKey == "textures.ao")
				        {
					        spec.occlusionPath = ResolvePresetPath(path, entry.value);
					        return;
				        }
				        if (entry.fullKey == "textures.emissive" || entry.fullKey == "emissive")
				        {
					        spec.emissivePath = ResolvePresetPath(path, entry.value);
				        }
			        });

			return spec;
		}

		Expected<std::string> ReadTextFile(std::string_view path)
		{
			AE_TRY(bytes, io::FileSystem::ReadFile(path));
			std::string text;
			text.resize(bytes->size());
			for (std::size_t i = 0; i < bytes->size(); ++i)
			{
				text[i] = static_cast<char>((*bytes)[i]);
			}
			return text;
		}
	} // namespace

	void AssetManager::Initialize(VulkanContext& context, BindlessManager& bindlessManager, MaterialRegistry& materialRegistry, World& world, gpu::UploadContext& uploadContext)
	{
		AE_PROFILE_ZONE();
		m_context = &context;
		m_bindlessManager = &bindlessManager;
		m_materialRegistry = &materialRegistry;
		m_world = &world;
		m_uploadContext = &uploadContext;
		MaterialSystem::ConnectLifecycle(world, materialRegistry);
	}

	Mesh AssetManager::CreateMesh(std::span<const Mesh::Vertex> vertices)
	{
		return Mesh::Create(*m_uploadContext, vertices);
	}

	Mesh AssetManager::CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices)
	{
		return Mesh::Create(*m_uploadContext, vertices, indices);
	}

	Mesh AssetManager::CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices, const float* aabbMin, const float* aabbMax, const float* sphereCenter, float sphereRadius)
	{
		return Mesh::Create(*m_uploadContext, vertices, indices, aabbMin, aabbMax, sphereCenter, sphereRadius);
	}

	Expected<Texture> AssetManager::CreateTexture(std::string_view path)
	{
		return Texture::LoadFromFile(path, m_context->GetDevice().device, m_context->GetGraphicsQueue(), m_uploadContext->GetCommandPool());
	}

	coro::async<Expected<Texture>> AssetManager::CreateTextureAsync(std::string_view path)
	{
		AE_PROFILE_ZONE();
		// Read file data on the I/O thread (suspends the calling coroutine).
		const std::string pathStr(path);
		auto fileData = co_await io::FileSystem::ReadFileAsync(pathStr);

		// GPU upload must happen on the game thread (owns the Vulkan context).
		// After co_await resumes, we're back on the game thread via the default
		// executor, so this is safe.
		co_return Texture::LoadFromFileData(fileData, pathStr, m_context->GetDevice().device, m_context->GetGraphicsQueue(), m_uploadContext->GetCommandPool());
	}

	Expected<GraphicsPipeline> AssetManager::CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc)
	{
		return GraphicsPipeline::Create(m_context->GetDevice().device, desc);
	}

	Expected<MaterialAsset> AssetManager::LoadMaterialPreset(std::string_view path, std::vector<Texture>& outTextures)
	{
		AE_PROFILE_ZONE();
		const std::string requestedPath = NormalizeVirtualFolder(std::string(path));

		std::string presetPath = requestedPath;
		std::string folderPath;
		const std::string folderTomlPath = requestedPath + "/properties.toml";
		if (io::FileSystem::Exists(folderTomlPath))
		{
			presetPath = folderTomlPath;
			folderPath = requestedPath;
		}

		if (!io::FileSystem::Exists(presetPath))
		{
			AE_UNEXPECTED(AetherError::Asset("file/folder not found: " + requestedPath));
		}

		// Try binary .material first, fall back to TOML.
		const bool isBinary = presetPath.ends_with(".material");
		if (isBinary)
		{
			auto data = io::FileSystem::ReadFile(presetPath);
			if (data.has_value())
			{
				BinaryReader reader(*data);
				auto hdr = reader.Read<MaterialHeaderDisk>();
				if (CheckMagic(hdr))
				{
					MaterialAsset material;
					material.baseColorFactor = glm::vec4(hdr.baseColorFactor[0], hdr.baseColorFactor[1], hdr.baseColorFactor[2], hdr.baseColorFactor[3]);
					material.metallicFactor = hdr.metallicFactor;
					material.roughnessFactor = hdr.roughnessFactor;
					material.emissiveFactor = glm::vec3(hdr.emissiveFactor[0], hdr.emissiveFactor[1], hdr.emissiveFactor[2]);
					material.alphaCutoff = hdr.alphaCutoff;
					material.doubleSided = hdr.doubleSided != 0;
					material.alphaBlend = hdr.alphaBlend != 0;
					material.alphaMask = hdr.alphaMask != 0;

					// Resolve texture paths.
					for (uint8_t t = 0; t < hdr.texturePathCount; ++t)
					{
						auto type = reader.Read<uint8_t>();
						std::string texRelPath = reader.ReadString();
						std::string texPath = ResolvePathRelativeTo(presetPath, texRelPath);

						// Try .texture sibling first.
						if (!io::FileSystem::Exists(texPath))
						{
							const std::size_t ss = texPath.find("://");
							if (ss != std::string_view::npos)
							{
								const std::filesystem::path rel(texPath.substr(ss + 3));
								const std::string candidate = texPath.substr(0, ss) + "://" + (rel.parent_path() / rel.stem()).generic_string() + ".texture";
								if (io::FileSystem::Exists(candidate))
								{
									texPath = candidate;
								}
							}
						}

						auto loadSlot = [this, &outTextures, &texPath]() -> std::uint32_t
						{
							if (!io::FileSystem::Exists(texPath))
							{
								return MaterialAsset::kNoTexture;
							}
							auto texResult = CreateTexture(texPath);
							if (!texResult.has_value())
							{
								return MaterialAsset::kNoTexture;
							}
							const std::uint32_t slot = texResult->GetBindlessSlot();
							outTextures.push_back(std::move(*texResult));
							return slot;
						};

						auto texType = static_cast<TextureTypeDisk>(type);
						switch (texType)
						{
							case TextureTypeDisk::BaseColor:
								material.albedoSlot = loadSlot();
								break;
							case TextureTypeDisk::Normal:
								material.normalSlot = loadSlot();
								break;
							case TextureTypeDisk::MetallicRoughness:
								material.metallicRoughnessSlot = loadSlot();
								break;
							case TextureTypeDisk::Occlusion:
								material.occlusionSlot = loadSlot();
								break;
							case TextureTypeDisk::Emissive:
								material.emissiveSlot = loadSlot();
								break;
						}
					}

					AE_INFO(LogCategory::Engine, "Loaded binary material '{}'.", requestedPath);
					return material;
				}
			}
		}

		// TOML fallback.
		AE_TRY(text, ReadTextFile(presetPath));
		const MaterialPresetSpec spec = ParseMaterialPreset(presetPath, *text);
		MaterialAsset material = spec.material;

		auto loadTextureSlot = [this, &outTextures](std::string_view texturePath) -> Expected<std::uint32_t>
		{
			if (texturePath.empty())
			{
				return MaterialAsset::kNoTexture;
			}

			// If the original path doesn't exist, try the pre-transcoded .texture sibling.
			std::string resolvedPath(texturePath);
			if (!io::FileSystem::Exists(resolvedPath))
			{
				const std::size_t ss = resolvedPath.find("://");
				if (ss != std::string::npos)
				{
					const std::filesystem::path rel(resolvedPath.substr(ss + 3));
					const std::string candidate = resolvedPath.substr(0, ss) + "://" + (rel.parent_path() / rel.stem()).generic_string() + ".texture";
					if (io::FileSystem::Exists(candidate))
					{
						resolvedPath = candidate;
					}
				}
			}

			if (!io::FileSystem::Exists(resolvedPath))
			{
				AE_WARN(LogCategory::Engine, "LoadMaterialPreset: texture missing '{}'.", texturePath);
				return MaterialAsset::kNoTexture;
			}

			AE_TRY(tex, CreateTexture(resolvedPath));
			const std::uint32_t slot = tex->GetBindlessSlot();
			outTextures.push_back(std::move(*tex));
			return slot;
		};

		std::string autoAlbedo;
		std::string autoNormal;
		std::string autoMetallicRoughness;
		std::string autoOcclusion;
		std::string autoEmissive;
		if (!folderPath.empty())
		{
			autoAlbedo = ResolveFirstAliasInFolder(folderPath, {"albedo", "basecolor", "base_color", "diffuse", "color"});
			autoNormal = ResolveFirstAliasInFolder(folderPath, {"normal", "nrm"});
			autoMetallicRoughness = ResolveFirstAliasInFolder(folderPath, {"metallicroughness", "metal_rough", "metalrough", "orm", "roughness", "metallic"});
			autoOcclusion = ResolveFirstAliasInFolder(folderPath, {"occlusion", "ao", "ambientocclusion"});
			autoEmissive = ResolveFirstAliasInFolder(folderPath, {"emissive", "emission"});
		}

		const std::string albedoPath = !spec.albedoPath.empty() ? spec.albedoPath : autoAlbedo;
		const std::string normalPath = !spec.normalPath.empty() ? spec.normalPath : autoNormal;
		const std::string metallicRoughnessPath = !spec.metallicRoughnessPath.empty() ? spec.metallicRoughnessPath : autoMetallicRoughness;
		const std::string occlusionPath = !spec.occlusionPath.empty() ? spec.occlusionPath : autoOcclusion;
		const std::string emissivePath = !spec.emissivePath.empty() ? spec.emissivePath : autoEmissive;

		AE_TRY(albedoSlot, loadTextureSlot(albedoPath));
		material.albedoSlot = *albedoSlot;
		AE_TRY(normalSlot, loadTextureSlot(normalPath));
		material.normalSlot = *normalSlot;
		AE_TRY(metallicRoughnessSlot, loadTextureSlot(metallicRoughnessPath));
		material.metallicRoughnessSlot = *metallicRoughnessSlot;
		AE_TRY(occlusionSlot, loadTextureSlot(occlusionPath));
		material.occlusionSlot = *occlusionSlot;
		AE_TRY(emissiveSlot, loadTextureSlot(emissivePath));
		material.emissiveSlot = *emissiveSlot;

		AE_INFO(LogCategory::Engine,
		        "Loaded material preset '{}' (albedo={}, normal={}, metallicRoughness={}, occlusion={}, emissive={}).",
		        requestedPath,
		        material.albedoSlot != MaterialAsset::kNoTexture ? "yes" : "no",
		        material.normalSlot != MaterialAsset::kNoTexture ? "yes" : "no",
		        material.metallicRoughnessSlot != MaterialAsset::kNoTexture ? "yes" : "no",
		        material.occlusionSlot != MaterialAsset::kNoTexture ? "yes" : "no",
		        material.emissiveSlot != MaterialAsset::kNoTexture ? "yes" : "no");

		return material;
	}

	Expected<LoadedModel> AssetManager::LoadModel(std::string_view path)
	{
		AE_PROFILE_ZONE();
		AE_VERBOSE(LogCategory::Engine, "Loading model: {}", path);
		AE_TRY(source, assets::GltfAsset::LoadFromVfsPath(path));
		LoadedModel loaded;

		AE_VERBOSE(LogCategory::Engine, "Model parsed: {} nodes, {} primitives, {} skins, {} materials, {} animations", source->nodes.size(), source->primitives.size(), source->skins.size(), source->materials.size(), source->animations.size());

		// New format: materials have texture paths, not embedded images.
		// Textures are loaded in FinaliseModelLoad.
		std::vector<std::uint32_t> imageSlots;

		FinaliseModelLoad(loaded, *source, imageSlots, path);
		return std::move(loaded);
	}

	coro::async<Expected<LoadedModel>> AssetManager::LoadModelAsync(std::string_view path)
	{
		AE_PROFILE_ZONE();
		const std::string pathStr(path);

		// Resolve the .mesh path (same logic as GltfAsset::LoadFromVfsPath).
		std::string meshPath = assets::GltfAsset::ResolveMeshPath(pathStr);
		if (meshPath.empty() || !io::FileSystem::Exists(meshPath))
		{
			meshPath = std::string(path);
		}

		// Read the .mesh file on the I/O thread.
		auto meshData = co_await io::FileSystem::ReadFileAsync(meshPath);

		// Parse from memory on the game thread (after resumption).
		auto source = assets::GltfAsset::LoadFromMemory(std::move(meshData), meshPath);
		if (!source.has_value())
		{
			co_return std::unexpected(source.error());
		}
		LoadedModel loaded;

		// New format: textures are loaded in FinaliseModelLoad from material paths.
		std::vector<std::uint32_t> imageSlots;

		FinaliseModelLoad(loaded, *source, imageSlots, pathStr);
		co_return std::move(loaded);
	}

	void AssetManager::FinaliseModelLoad(LoadedModel& loaded, const assets::GltfAsset& source, const std::vector<std::uint32_t>& imageSlots, std::string_view path)
	{
		AE_PROFILE_ZONE();
		AE_VERBOSE(LogCategory::Engine, "Finalising model: {} nodes, {} primitives, {} skins, {} materials", source.nodes.size(), source.primitives.size(), source.skins.size(), source.materials.size());

		std::vector<glm::mat4> localNodeTransforms(source.nodes.size(), glm::mat4(1.0f));
		for (std::size_t nodeIndex = 0; nodeIndex < source.nodes.size(); ++nodeIndex)
		{
			const assets::GltfNode& node = source.nodes[nodeIndex];
			if (node.hasMatrix)
			{
				localNodeTransforms[nodeIndex] = node.matrix;
				continue;
			}

			const glm::mat4 t = glm::translate(glm::mat4(1.0f), node.translation);
			const glm::mat4 r = glm::mat4_cast(node.rotation);
			const glm::mat4 s = glm::scale(glm::mat4(1.0f), node.scale);
			localNodeTransforms[nodeIndex] = t * r * s;
		}

		std::vector<glm::mat4> worldNodeTransforms(source.nodes.size(), glm::mat4(1.0f));
		for (std::size_t nodeIndex = 0; nodeIndex < source.nodes.size(); ++nodeIndex)
		{
			glm::mat4 transform = localNodeTransforms[nodeIndex];
			std::int32_t parent = source.nodes[nodeIndex].parentIndex;
			while (parent >= 0)
			{
				transform = localNodeTransforms[static_cast<std::size_t>(parent)] * transform;
				parent = source.nodes[static_cast<std::size_t>(parent)].parentIndex;
			}
			worldNodeTransforms[nodeIndex] = transform;
		}

		// Helper: load a texture from a VFS path, trying .texture sibling first.
		auto loadTextureFromPath = [this, &loaded](std::string_view texturePath) -> std::uint32_t
		{
			if (texturePath.empty())
			{
				return MaterialAsset::kNoTexture;
			}

			std::string resolvedPath(texturePath);
			// Try .texture sibling first (pre-transcoded DDS).
			if (!io::FileSystem::Exists(resolvedPath))
			{
				const std::size_t ss = resolvedPath.find("://");
				if (ss != std::string_view::npos)
				{
					const std::filesystem::path rel(resolvedPath.substr(ss + 3));
					const std::string candidate = resolvedPath.substr(0, ss) + "://" + (rel.parent_path() / rel.stem()).generic_string() + ".texture";
					if (io::FileSystem::Exists(candidate))
					{
						AE_VERBOSE(LogCategory::Engine, "    Texture resolved: {} -> {}", texturePath, candidate);
						resolvedPath = candidate;
					}
				}
			}

			if (!io::FileSystem::Exists(resolvedPath))
			{
				AE_WARN(LogCategory::Engine, "FinaliseModelLoad: texture missing '{}'.", texturePath);
				return MaterialAsset::kNoTexture;
			}

			auto texResult = CreateTexture(resolvedPath);
			if (!texResult.has_value())
			{
				AE_WARN(LogCategory::Engine, "FinaliseModelLoad: texture load failed '{}'.", resolvedPath);
				return MaterialAsset::kNoTexture;
			}

			AE_VERBOSE(LogCategory::Engine, "    Texture loaded: {} (slot={})", resolvedPath, texResult->GetBindlessSlot());
			const std::uint32_t slot = texResult->GetBindlessSlot();
			loaded.textures.push_back(std::move(*texResult));
			return slot;
		};

		loaded.primitives.reserve(source.primitives.size());
		for (std::size_t primIdx = 0; primIdx < source.primitives.size(); ++primIdx)
		{
			const assets::GltfPrimitive& primitive = source.primitives[primIdx];
			if (primitive.vertices.empty())
			{
				continue;
			}

			AE_VERBOSE(LogCategory::Engine, "  Primitive[{}]: {} verts, {} indices, skinIdx={}, matIdx={}", primIdx, primitive.vertices.size(), primitive.indices.size(), primitive.skinIndex, primitive.materialIndex);

			LoadedModelPrimitive loadedPrim;
			loadedPrim.mesh = CreateMesh(primitive.vertices, primitive.indices, primitive.aabbMin, primitive.aabbMax, primitive.sphereCenter, primitive.sphereRadius);
			loadedPrim.skinIndex = primitive.skinIndex;

			if (primitive.skinIndex < 0 && primitive.nodeIndex < worldNodeTransforms.size())
			{
				loadedPrim.localTransform = worldNodeTransforms[primitive.nodeIndex];
			}

			if (primitive.materialIndex >= 0 && static_cast<std::size_t>(primitive.materialIndex) < source.materials.size())
			{
				const assets::GltfMaterial& srcMat = source.materials[static_cast<std::size_t>(primitive.materialIndex)];
				MaterialAsset& mat = loadedPrim.material;
				loadedPrim.hasMaterial = true;

				AE_VERBOSE(LogCategory::Engine, "  Material '{}': albedo='{}', normal='{}', orm='{}'", srcMat.name, srcMat.albedoPath, srcMat.normalPath, srcMat.metallicRoughnessPath);

				mat.baseColorFactor = srcMat.baseColorFactor;
				mat.metallicFactor = srcMat.metallicFactor;
				mat.roughnessFactor = srcMat.roughnessFactor;
				mat.emissiveFactor = srcMat.emissiveFactor;
				mat.alphaCutoff = srcMat.alphaCutoff;
				mat.doubleSided = srcMat.doubleSided;
				mat.alphaBlend = srcMat.alphaBlend;
				mat.alphaMask = srcMat.alphaMask;

				// New format: resolve texture paths from GltfMaterial.
				// Old format: resolve via imageSlots index chain.
				if (imageSlots.empty())
				{
					mat.albedoSlot = loadTextureFromPath(srcMat.albedoPath);
					mat.normalSlot = loadTextureFromPath(srcMat.normalPath);
					mat.metallicRoughnessSlot = loadTextureFromPath(srcMat.metallicRoughnessPath);
					mat.occlusionSlot = loadTextureFromPath(srcMat.occlusionPath);
					mat.emissiveSlot = loadTextureFromPath(srcMat.emissivePath);
				}
				else
				{
					auto resolveSlot = [&](const std::int32_t texIdx) -> std::uint32_t
					{
						if (texIdx < 0 || static_cast<std::size_t>(texIdx) >= source.textures.size())
						{
							return MaterialAsset::kNoTexture;
						}
						const assets::GltfTexture& tex = source.textures[static_cast<std::size_t>(texIdx)];
						if (tex.imageIndex < 0 || static_cast<std::size_t>(tex.imageIndex) >= imageSlots.size())
						{
							return MaterialAsset::kNoTexture;
						}
						return imageSlots[static_cast<std::size_t>(tex.imageIndex)];
					};

					mat.albedoSlot = resolveSlot(srcMat.baseColorTexture);
					mat.normalSlot = resolveSlot(srcMat.normalTexture);
					mat.metallicRoughnessSlot = resolveSlot(srcMat.metallicRoughnessTexture);
					mat.occlusionSlot = resolveSlot(srcMat.occlusionTexture);
					mat.emissiveSlot = resolveSlot(srcMat.emissiveTexture);
				}

				AE_VERBOSE(LogCategory::Engine, "  Material slots: albedo={}, normal={}, orm={}, occlusion={}, emissive={}", mat.albedoSlot, mat.normalSlot, mat.metallicRoughnessSlot, mat.occlusionSlot, mat.emissiveSlot);
			}
			else
			{
				AE_WARN(LogCategory::Engine, "  Primitive[{}]: no material (matIdx={}, numMats={})", primIdx, primitive.materialIndex, source.materials.size());
			}

			loaded.primitives.push_back(std::move(loadedPrim));
		}

		AE_VERBOSE(LogCategory::Engine, "Loaded model '{}': {} primitive(s), {} texture(s), {} animation(s).", std::string(path), loaded.primitives.size(), loaded.textures.size(), source.animations.size());

		if (!source.skins.empty() && !source.animations.empty())
		{
			AE_VERBOSE(LogCategory::Engine, "  Creating animation database: {} skins, {} animations, {} nodes", source.skins.size(), source.animations.size(), source.nodes.size());
			loaded.animationDb = AnimationDatabase::Create(*m_context, m_uploadContext->GetCommandPool(), source);
			if (loaded.animationDb.IsValid())
			{
				AE_VERBOSE(LogCategory::Engine, "  Animation database created: {} clips, {} nodes, {} skins", loaded.animationDb.GetClipCount(), loaded.animationDb.GetNodeCount(), loaded.animationDb.GetSkinCount());
			}
			else
			{
				AE_WARN(LogCategory::Engine, "  Animation database creation returned empty (no clips?)");
			}
		}
		else
		{
			AE_INFO(LogCategory::Engine, "  No animation database: skins={}, animations={}", source.skins.size(), source.animations.size());
		}
	}

	std::vector<Entity> AssetManager::SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, std::uint32_t parentEntityId, float scale)
	{
		AE_PROFILE_ZONE();
		std::vector<Entity> entities;
		entities.reserve(model.primitives.size());

		if (model.animationDb.IsValid())
		{
			m_renderQueue->SetAnimationDatabase(&model.animationDb);
			m_shadowService->SetAnimationDatabase(&model.animationDb);
			m_renderTargetService->SetAnimationDatabase(&model.animationDb);
		}

		const glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), glm::vec3(scale));

		for (const LoadedModelPrimitive& primitive: model.primitives)
		{
			const Entity entity = primitive.hasMaterial ? aether::ecs::SpawnMesh(*m_world, pipeline, primitive.mesh, *m_materialRegistry, primitive.material, scaleMat * primitive.localTransform)
			                                            : aether::ecs::SpawnMesh(*m_world, pipeline, primitive.mesh, scaleMat * primitive.localTransform);

			if (parentEntityId != 0)
			{
				m_world->Emplace<ParentEntityComponent>(entity, ParentEntityComponent{.parentId = parentEntityId});
			}

			if (model.animationDb.IsValid() && primitive.skinIndex >= 0)
			{
				const auto skinIdx = static_cast<std::uint32_t>(primitive.skinIndex);
				const std::uint32_t joints = model.animationDb.GetSkinJointCount(skinIdx);
				if (joints > 0)
				{
					m_world->EmplaceOrReplace<SkinnedMeshComponent>(entity,
					        SkinnedMeshComponent{
					                .animDb = &model.animationDb,
					                .skinIndex = skinIdx,
					                .jointCount = joints,
					        });
				}
			}

			entities.push_back(entity);
		}

		return entities;
	}
} // namespace aether
