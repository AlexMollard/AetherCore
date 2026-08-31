#include "assets/AssetManager.hpp"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include <BinaryFormats.hpp>

#include "assets/GltfAsset.hpp"
#include "gpu/BindlessManager.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/Hierarchy.hpp"
#include "io/FileSystem.hpp"
#include "io/FileGlobOptions.hpp"
#include "utils/BinaryReader.hpp"
#include "utils/Logger.hpp"
#include "material/MaterialSerializer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/PipelineCache.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/LoadedModel.hpp"
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
		std::string NormalizeVirtualFolder(std::string path)
		{
			while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
			{
				path.pop_back();
			}
			return path;
		}

		std::string ResolvePathInFolder(std::string_view folderPath, const std::string& resourcePath)
		{
			if (resourcePath.empty())
			{
				return {};
			}
			if (resourcePath.contains("://"))
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

			if (stem.contains("://"))
			{
				return io::FileSystem::Exists(stem) ? stem : std::string();
			}

			std::string direct = ResolvePathInFolder(folderPath, stem);
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

	void AssetManager::Initialize(VulkanContext& context,
	        BindlessManager& bindlessManager,
	        MaterialRegistry& materialRegistry,
	        MaterialAuthoring& materialAuthoring,
	        PipelineCache& pipelineCache,
	        EffectParamBuffer& effectParamBuffer,
	        TextureRegistry& textureRegistry,
	        World& world,
	        gpu::UploadContext& uploadContext)
	{
		AE_PROFILE_ZONE();
		m_context = &context;
		m_bindlessManager = &bindlessManager;
		m_materialRegistry = &materialRegistry;
		m_materialAuthoring = &materialAuthoring;
		m_pipelineCache = &pipelineCache;
		m_effectParamBuffer = &effectParamBuffer;
		m_textureRegistry = &textureRegistry;
		m_world = &world;
		m_uploadContext = &uploadContext;
		MaterialSystem::ConnectLifecycle(world, materialRegistry);
	}

	void AssetManager::ReleaseModelTextures(LoadedModel& model)
	{
		if (m_textureRegistry == nullptr)
		{
			return;
		}
		for (LoadedModelPrimitive& prim: model.primitives)
		{
			MaterialAsset& mat = prim.material;
			const TextureHandle handles[5] = {mat.albedoTex, mat.normalTex, mat.metallicRoughnessTex, mat.occlusionTex, mat.emissiveTex};
			for (const TextureHandle h: handles)
			{
				m_textureRegistry->Release(h);
			}
			mat.albedoTex = {};
			mat.normalTex = {};
			mat.metallicRoughnessTex = {};
			mat.occlusionTex = {};
			mat.emissiveTex = {};
		}
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

	Expected<Texture> AssetManager::CreateTextureFromDisk(const std::filesystem::path& path)
	{
		return Texture::LoadFromDiskPath(path, m_context->GetDevice().device, m_context->GetGraphicsQueue(), m_uploadContext->GetCommandPool());
	}

	coro::async<Expected<Texture>> AssetManager::CreateTextureAsync(std::string_view path)
	{
		AE_PROFILE_ZONE();
		// Read file data on the I/O thread (suspends the calling coroutine).
		const std::string pathStr(path);
		auto fileData = co_await io::FileSystem::ReadFileAsync(pathStr);

		// GPU upload must happen on the game thread (owns the Vulkan context).
		co_return Texture::LoadFromFileData(fileData, pathStr, m_context->GetDevice().device, m_context->GetGraphicsQueue(), m_uploadContext->GetCommandPool());
	}

	Expected<GraphicsPipeline> AssetManager::CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc)
	{
		return GraphicsPipeline::Create(m_context->GetDevice().device, desc);
	}

	gpu::ResourceRegistry::PreparedPipeline AssetManager::PrepareGraphicsPipeline(const GraphicsPipeline::Desc& desc)
	{
		return GraphicsPipeline::Prepare(m_context->GetDevice().device, desc);
	}

	std::string_view AssetManager::InternShaderVfsPath(std::string path)
	{
		if (path.empty())
		{
			return {};
		}
		// unordered_set never relocates existing elements on insert, so the
		const auto [it, inserted] = m_internedShaderVfsPaths.insert(std::move(path));
		return *it;
	}

	Expected<MaterialAsset> AssetManager::LoadMaterialPreset(std::string_view path)
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

					for (uint8_t t = 0; t < hdr.texturePathCount; ++t)
					{
						auto type = reader.Read<uint8_t>();
						const std::string texRelPath = reader.ReadString();
						const std::string texPath = io::FileSystem::ResolveRelative(presetPath, texRelPath);

						auto texType = static_cast<TextureTypeDisk>(type);

						// Only base colour and emissive are sRGB-encoded; the rest are data.
						const TextureColorSpace colorSpace =
						        (texType == TextureTypeDisk::BaseColor || texType == TextureTypeDisk::Emissive) ? TextureColorSpace::Srgb : TextureColorSpace::Linear;
						const TextureHandle tex = m_textureRegistry->Acquire(texPath, colorSpace);

						switch (texType)
						{
							case TextureTypeDisk::BaseColor:
								material.albedoTex = tex;
								break;
							case TextureTypeDisk::Normal:
								material.normalTex = tex;
								break;
							case TextureTypeDisk::MetallicRoughness:
								material.metallicRoughnessTex = tex;
								break;
							case TextureTypeDisk::Occlusion:
								material.occlusionTex = tex;
								break;
							case TextureTypeDisk::Emissive:
								material.emissiveTex = tex;
								break;
						}
					}

					std::string shaderVfsPath = reader.ReadString();
					if (!shaderVfsPath.empty())
					{
						material.templateDesc.shaderVfsPath = InternShaderVfsPath(std::move(shaderVfsPath));
					}

					AE_INFO(LogCategory::Engine, "Loaded binary material '{}'.", requestedPath);
					return material;
				}
			}
		}

		AE_TRY(text, ReadTextFile(presetPath));
		const MaterialPresetSpec spec = MaterialSerializer::Parse(presetPath, *text);
		MaterialAsset material = spec.material;
		if (!spec.shaderVfsPath.empty())
		{
			material.templateDesc.shaderVfsPath = InternShaderVfsPath(spec.shaderVfsPath);
		}

		auto acquireTexture = [this](std::string_view texturePath, TextureColorSpace colorSpace) -> TextureHandle
		{
			if (texturePath.empty())
			{
				return {};
			}
			return m_textureRegistry->Acquire(texturePath, colorSpace);
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

		// Base colour and emissive carry light and are sRGB-encoded. The rest carry
		// numbers - directions, roughness, occlusion - and must not be run through the
		// sampler's sRGB decode.
		material.albedoTex = acquireTexture(albedoPath, TextureColorSpace::Srgb);
		material.normalTex = acquireTexture(normalPath, TextureColorSpace::Linear);
		material.metallicRoughnessTex = acquireTexture(metallicRoughnessPath, TextureColorSpace::Linear);
		material.occlusionTex = acquireTexture(occlusionPath, TextureColorSpace::Linear);
		material.emissiveTex = acquireTexture(emissivePath, TextureColorSpace::Srgb);

		AE_INFO(LogCategory::Engine,
		        "Loaded material preset '{}' (albedo={}, normal={}, metallicRoughness={}, occlusion={}, emissive={}).",
		        requestedPath,
		        material.albedoTex.IsValid() ? "yes" : "no",
		        material.normalTex.IsValid() ? "yes" : "no",
		        material.metallicRoughnessTex.IsValid() ? "yes" : "no",
		        material.occlusionTex.IsValid() ? "yes" : "no",
		        material.emissiveTex.IsValid() ? "yes" : "no");

		return material;
	}

	Expected<LoadedModel> AssetManager::LoadModel(std::string_view path)
	{
		AE_PROFILE_ZONE();
		AE_VERBOSE(LogCategory::Engine, "Loading model: {}", path);
		AE_TRY(source, assets::GltfAsset::LoadFromVfsPath(path));
		LoadedModel loaded;

		AE_VERBOSE(LogCategory::Engine, "Model parsed: {} nodes, {} primitives, {} skins, {} materials, {} animations", source->nodes.size(), source->primitives.size(), source->skins.size(), source->materials.size(), source->animations.size());

		const std::vector<TextureHandle> imageHandles;

		FinaliseModelLoad(loaded, *source, imageHandles, path);
		return std::move(loaded);
	}

	coro::async<Expected<LoadedModel>> AssetManager::LoadModelAsync(std::string_view path)
	{
		AE_PROFILE_ZONE();
		const std::string pathStr(path);

		std::string meshPath = assets::GltfAsset::ResolveMeshPath(pathStr);
		if (meshPath.empty() || !io::FileSystem::Exists(meshPath))
		{
			meshPath = std::string(path);
		}

		// Read the .mesh file on the I/O thread.
		auto meshData = co_await io::FileSystem::ReadFileAsync(meshPath);

		// Parse from memory on the game thread (after resumption).
		auto source = assets::GltfAsset::LoadFromMemory(meshData, meshPath);
		if (!source.has_value())
		{
			co_return std::unexpected(source.error());
		}
		LoadedModel loaded;

		const std::vector<TextureHandle> imageHandles;

		FinaliseModelLoad(loaded, *source, imageHandles, pathStr);
		co_return std::move(loaded);
	}

	void AssetManager::FinaliseModelLoad(LoadedModel& loaded, const assets::GltfAsset& source, const std::vector<TextureHandle>& imageHandles, std::string_view path)
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

		auto acquireTexture = [this](std::string_view texturePath, TextureColorSpace colorSpace) -> TextureHandle
		{
			if (texturePath.empty())
			{
				return {};
			}
			return m_textureRegistry->Acquire(texturePath, colorSpace);
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

				// material's properties.toml. Intern into AssetManager-lifetime
				if (!srcMat.shaderVfsPath.empty())
				{
					mat.templateDesc.shaderVfsPath = InternShaderVfsPath(srcMat.shaderVfsPath);
				}

				if (imageHandles.empty())
				{
					mat.albedoTex = acquireTexture(srcMat.albedoPath, TextureColorSpace::Srgb);
					mat.normalTex = acquireTexture(srcMat.normalPath, TextureColorSpace::Linear);
					mat.metallicRoughnessTex = acquireTexture(srcMat.metallicRoughnessPath, TextureColorSpace::Linear);
					mat.occlusionTex = acquireTexture(srcMat.occlusionPath, TextureColorSpace::Linear);
					mat.emissiveTex = acquireTexture(srcMat.emissivePath, TextureColorSpace::Srgb);
				}
				else
				{
					auto resolveHandle = [&](const std::int32_t texIdx) -> TextureHandle
					{
						if (texIdx < 0 || static_cast<std::size_t>(texIdx) >= source.textures.size())
						{
							return {};
						}
						const assets::GltfTexture& tex = source.textures[static_cast<std::size_t>(texIdx)];
						if (tex.imageIndex < 0 || static_cast<std::size_t>(tex.imageIndex) >= imageHandles.size())
						{
							return {};
						}
						return imageHandles[static_cast<std::size_t>(tex.imageIndex)];
					};

					mat.albedoTex = resolveHandle(srcMat.baseColorTexture);
					mat.normalTex = resolveHandle(srcMat.normalTexture);
					mat.metallicRoughnessTex = resolveHandle(srcMat.metallicRoughnessTexture);
					mat.occlusionTex = resolveHandle(srcMat.occlusionTexture);
					mat.emissiveTex = resolveHandle(srcMat.emissiveTexture);
				}

				AE_VERBOSE(LogCategory::Engine,
				        "  Material textures present: albedo={}, normal={}, orm={}, occlusion={}, emissive={}",
				        mat.albedoTex.IsValid(),
				        mat.normalTex.IsValid(),
				        mat.metallicRoughnessTex.IsValid(),
				        mat.occlusionTex.IsValid(),
				        mat.emissiveTex.IsValid());
			}
			else
			{
				AE_WARN(LogCategory::Engine, "  Primitive[{}]: no material (matIdx={}, numMats={})", primIdx, primitive.materialIndex, source.materials.size());
			}

			loaded.primitives.push_back(std::move(loadedPrim));
		}

		AE_VERBOSE(LogCategory::Engine, "Loaded model '{}': {} primitive(s), {} animation(s).", std::string(path), loaded.primitives.size(), source.animations.size());

		if (!source.skins.empty() && !source.animations.empty())
		{
			AE_VERBOSE(LogCategory::Engine, "  Creating animation database: {} skins, {} animations, {} nodes", source.skins.size(), source.animations.size(), source.nodes.size());
			loaded.animationDb = AnimationDatabase::Create(*m_context, source);
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

	std::vector<Entity> AssetManager::SpawnModel(LoadedModel& model, std::uint32_t parentEntityId, float scale)
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

		MaterialTemplate defaultTemplate{.shaderVfsPath = "shaders://gltf_mesh.spv"};
		defaultTemplate.cullMode = gpu::CullMode::None;
		const GraphicsPipeline* defaultPipeline = m_pipelineCache->Acquire(defaultTemplate);

		for (const LoadedModelPrimitive& primitive: model.primitives)
		{
			const Entity entity = primitive.hasMaterial ? aether::ecs::SpawnMesh(*m_world, primitive.mesh, *m_materialRegistry, *m_pipelineCache, primitive.material, scaleMat * primitive.localTransform)
			                                            : aether::ecs::SpawnMesh(*m_world, defaultPipeline, primitive.mesh, scaleMat * primitive.localTransform);

			if (parentEntityId != 0)
			{
				aether::ecs::SetParent(*m_world, entity, Entity{parentEntityId});
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
