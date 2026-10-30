#include "assets/AssetManager.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <exception>
#include <stdexcept>

#include "assets/GltfAsset.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/BindlessManager.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/Hierarchy.hpp"
#include "io/FileSystem.hpp"
#include "io/FileGlobOptions.hpp"
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
#include "vulkan/VulkanContext.hpp"
#include "vulkan/TransferManager.hpp"
#include "scene/World.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
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

	bool AssetManager::HasLiveModelUsers(const LoadedModel& model) const
	{
		if (!m_world)
			return true;
		auto& registry = m_world->GetRegistry();
		for (auto&& [entity, component] : registry.view<MeshComponent>().each())
		{
			for (const auto& primitive : model.primitives)
				if (component.mesh == &primitive.mesh)
					return true;
		}
		for (auto&& [entity, component] : registry.view<SkinnedMeshComponent>().each())
			if (component.animDb == &model.animationDb)
				return true;
		return false;
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
		const std::string pathStr = Texture::ResolveTexturePath(io::FileSystem::ResolveAssetPath(path));
		auto pair = coro::task<Expected<Texture::Prepared>>::create();
		io::FileSystem::Submit(io::IOPriority::Normal,
		        [pathStr, completion = std::move(pair.second)]() mutable
		        {
			        try
			        {
				        auto data = io::FileSystem::ReadFile(pathStr);
				        if (!data)
					        completion.set_value(std::unexpected(data.error()));
				        else
					        completion.set_value(Texture::Decode(*data, pathStr));
			        }
			        catch (...)
			        {
				        completion.set_exception(std::current_exception());
			        }
		        });
		auto prepared = co_await std::move(pair.first);
		if (!prepared)
			co_return std::unexpected(prepared.error());
		co_return Texture::Upload(*prepared, pathStr, m_context->GetDevice().device, m_context->GetGraphicsQueue(), m_uploadContext->GetCommandPool());
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
		if (auto material = TryLoadStreamedMaterialPreset(path)) return std::move(*material);
		AE_TRY(spec, MaterialSerializer::Load(path));
		MaterialAsset material = spec->material;
		if (!spec->shaderVfsPath.empty())
		{
			material.templateDesc.shaderVfsPath = InternShaderVfsPath(spec->shaderVfsPath);
		}

		auto acquireTexture = [this](std::string_view texturePath, TextureColorSpace colorSpace) -> TextureHandle
		{
			if (texturePath.empty())
			{
				return {};
			}
			return m_textureRegistry->Acquire(texturePath, colorSpace);
		};

		// Base colour and emissive carry light and are sRGB-encoded. The rest carry
		// numbers - directions, roughness, occlusion - and must not be run through the
		// sampler's sRGB decode.
		material.albedoTex = acquireTexture(spec->albedoPath, TextureColorSpace::Srgb);
		material.normalTex = acquireTexture(spec->normalPath, TextureColorSpace::Linear);
		material.metallicRoughnessTex = acquireTexture(spec->metallicRoughnessPath, TextureColorSpace::Linear);
		material.occlusionTex = acquireTexture(spec->occlusionPath, TextureColorSpace::Linear);
		material.emissiveTex = acquireTexture(spec->emissivePath, TextureColorSpace::Srgb);

		AE_INFO(LogCategory::Engine,
		        "Loaded material preset '{}' (albedo={}, normal={}, metallicRoughness={}, occlusion={}, emissive={}).",
		        path,
		        material.albedoTex.IsValid() ? "yes" : "no",
		        material.normalTex.IsValid() ? "yes" : "no",
		        material.metallicRoughnessTex.IsValid() ? "yes" : "no",
		        material.occlusionTex.IsValid() ? "yes" : "no",
		        material.emissiveTex.IsValid() ? "yes" : "no");

		return material;
	}

	std::optional<MaterialAsset> AssetManager::TryLoadStreamedMaterialPreset(std::string_view path)
	{
		const auto resolved = io::FileSystem::ResolveAssetPath(path);
		if (resolved.empty()) return std::nullopt;
		const auto key = io::FileSystem::AssetReference(resolved);
		const auto it = m_streamedMaterials.find(key);
		if (it == m_streamedMaterials.end()) return std::nullopt;
		MaterialAsset material = it->second;
		for (auto handle : {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex})
			if (handle.IsValid()) m_textureRegistry->AddRef(handle);
		return material;
	}

	Expected<bool> AssetManager::PublishStreamedMaterial(std::string path, const MaterialPresetSpec& spec)
	{
		const auto resolved = io::FileSystem::ResolveAssetPath(path);
		if (resolved.empty()) AE_UNEXPECTED(AetherError::Asset("Unknown streamed material reference: " + path));
		path = io::FileSystem::AssetReference(resolved);
		if (m_streamedMaterials.contains(path)) return false;
		MaterialAsset material = spec.material;
		material.albedoTex = {};
		material.normalTex = {};
		material.metallicRoughnessTex = {};
		material.occlusionTex = {};
		material.emissiveTex = {};
		if (!spec.shaderVfsPath.empty()) material.templateDesc.shaderVfsPath = InternShaderVfsPath(spec.shaderVfsPath);
		const auto acquire = [this](const std::string& reference, TextureColorSpace colorSpace, TextureHandle& out)
		{
			if (reference.empty()) { out = {}; return true; }
			out = m_textureRegistry->FindAndAcquire(reference, colorSpace);
			return out.IsValid() && out != TextureHandle::Broken();
		};
		if (!acquire(spec.albedoPath, TextureColorSpace::Srgb, material.albedoTex) ||
		    !acquire(spec.normalPath, TextureColorSpace::Linear, material.normalTex) ||
		    !acquire(spec.metallicRoughnessPath, TextureColorSpace::Linear, material.metallicRoughnessTex) ||
		    !acquire(spec.occlusionPath, TextureColorSpace::Linear, material.occlusionTex) ||
		    !acquire(spec.emissivePath, TextureColorSpace::Srgb, material.emissiveTex))
		{
			for (auto handle : {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex})
				if (handle.IsValid() && handle != TextureHandle::Broken()) m_textureRegistry->Release(handle);
			AE_UNEXPECTED(AetherError::Asset("Prepared material texture is not resident."));
		}
		m_streamedMaterials.emplace(std::move(path), std::move(material));
		return true;
	}

	void AssetManager::RetireStreamedMaterial(std::string_view path)
	{
		const auto key = path.starts_with("uid://") ? std::string(path) : io::FileSystem::AssetReference(path);
		const auto it = m_streamedMaterials.find(key);
		if (it == m_streamedMaterials.end()) return;
		const auto& material = it->second;
		for (auto handle : {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex})
			if (handle.IsValid()) m_textureRegistry->Release(handle);
		m_streamedMaterials.erase(it);
	}

	Expected<LoadedModel> AssetManager::LoadModel(std::string_view path)
	{
		AE_PROFILE_ZONE();
		AE_VERBOSE(LogCategory::Engine, "Loading model: {}", path);
		AE_TRY(source, assets::GltfAsset::LoadFromVfsPath(path));
		LoadedModel loaded;

		AE_VERBOSE(LogCategory::Engine, "Model parsed: {} nodes, {} primitives, {} skins, {} materials, {} animations", source->nodes.size(), source->primitives.size(), source->skins.size(), source->materials.size(), source->animations.size());

		const std::vector<TextureHandle> imageHandles;

		(void) FinaliseModelLoad(loaded, *source, imageHandles, path);
		return std::move(loaded);
	}

	coro::async<Expected<LoadedModel>> AssetManager::LoadModelAsync(std::string_view path)
	{
		AE_PROFILE_ZONE();
		const std::string pathStr = io::FileSystem::ResolveAssetPath(path);
		if (pathStr.empty())
		{
			co_return std::unexpected(AetherError::Asset("Unknown asset UID: " + std::string(path)));
		}

		std::string meshPath = assets::GltfAsset::ResolveMeshPath(pathStr);
		if (meshPath.empty() || !io::FileSystem::Exists(meshPath))
		{
			meshPath = pathStr;
		}

		auto pair = coro::task<Expected<assets::GltfAsset>>::create();
		io::FileSystem::Submit(io::IOPriority::Normal,
		        [meshPath, completion = std::move(pair.second)]() mutable
		        {
			        try
			        {
				        auto data = io::FileSystem::ReadFile(meshPath);
				        if (!data)
					        completion.set_value(std::unexpected(data.error()));
				        else
					        completion.set_value(assets::GltfAsset::LoadFromMemory(*data, meshPath));
			        }
			        catch (...)
			        {
				        completion.set_exception(std::current_exception());
			        }
		        });
		auto source = co_await std::move(pair.first);
		if (!source.has_value())
		{
			co_return std::unexpected(source.error());
		}
		LoadedModel loaded;

		const std::vector<TextureHandle> imageHandles;

		(void) FinaliseModelLoad(loaded, *source, imageHandles, pathStr);
		co_return std::move(loaded);
	}

	Expected<AssetManager::ModelUploadSizes> AssetManager::PreparedModelUploadSizes(const assets::GltfAsset& source)
	{
		ModelUploadSizes sizes;
		bool failed = false;
		const auto addStaging = [&sizes, &failed](std::uint64_t bytes)
		{
			if (!bytes)
				return;
			gpu::MappedBufferDesc desc{};
			desc.size = bytes;
			desc.usage = gpu::BufferUsage::TransferSrc;
			const auto requirement = gpu::ResourceRegistry::GetMappedBufferAllocationRequirement(desc);
			failed |= requirement == 0;
			sizes.stagingBytes += requirement;
		};
		const auto addBuffer = [&sizes, &failed](std::uint64_t bytes, gpu::BufferUsage usage)
		{
			if (!bytes)
				return;
			gpu::BufferDesc desc{};
			desc.size = bytes;
			desc.usage = usage;
			const auto requirement = gpu::ResourceRegistry::GetBufferAllocationRequirement(desc);
			failed |= requirement == 0;
			sizes.gpuBytes += requirement;
		};
		for (const auto& primitive : source.primitives)
		{
			if (primitive.vertices.empty())
				continue;
			const std::uint64_t vertices = primitive.vertices.size() * sizeof(Mesh::Vertex);
			const std::uint64_t indices = primitive.indices.size() * sizeof(std::uint32_t);
			addBuffer(vertices, gpu::BufferUsage::Vertex | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress);
			addBuffer(indices, gpu::BufferUsage::Index | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress);
			addStaging(vertices);
			addStaging(indices);
		}
		if (source.animations.empty() || source.nodes.empty())
		{
			if (failed)
				return std::unexpected(AetherError::Engine("Failed to query model buffer allocation requirements"));
			return sizes;
		}

		std::uint64_t channels = 0, times = 0, values = 0, names = 0;
		for (const auto& clip : source.animations)
		{
			names += clip.name.size();
			channels += clip.channels.size();
			for (const auto& channel : clip.channels)
			{
				times += channel.times.size();
				values += channel.values.size();
			}
		}
		std::uint64_t joints = 0, inverseBinds = 0;
		for (const auto& skin : source.skins)
		{
			joints += skin.joints.size();
			inverseBinds += skin.inverseBindMatrices.empty() ? skin.joints.size() : skin.inverseBindMatrices.size();
		}
		std::uint64_t animationHeapBytes = 0;
		const auto addAnimation = [&animationHeapBytes, &addStaging](std::uint64_t bytes)
		{
			animationHeapBytes += (bytes + 15u) & ~std::uint64_t{15};
			addStaging(bytes);
		};
		addAnimation(source.animations.size() * sizeof(AnimationDatabase::GpuClip));
		addAnimation(channels * sizeof(AnimationDatabase::GpuChannel));
		addAnimation(times * sizeof(float));
		addAnimation(values * sizeof(glm::vec4));
		addAnimation(source.nodes.size() * sizeof(std::int32_t));
		for (int i = 0; i < 3; ++i)
			addAnimation(source.nodes.size() * sizeof(glm::vec4));
		addAnimation(source.skins.size() * sizeof(AnimationDatabase::GpuSkinMeta));
		addAnimation(joints * sizeof(std::uint32_t));
		addAnimation(inverseBinds * sizeof(glm::mat4));
		// Every node can occur once in the depth-sorted array and in at most one
		// depth range. This also covers disconnected/cyclic parent graphs.
		addAnimation(source.nodes.size() * sizeof(std::uint32_t));
		addAnimation(source.nodes.size() * sizeof(AnimationDatabase::DepthRange));
		addAnimation(names);
		addBuffer(animationHeapBytes, gpu::BufferUsage::Storage | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress);
		if (failed)
			return std::unexpected(AetherError::Engine("Failed to query model buffer allocation requirements"));
		return sizes;
	}

	struct AssetManager::PreparedModelUpload
	{
		AssetManager* manager = nullptr;
		const assets::GltfAsset* source = nullptr;
		std::string path;
		std::vector<Mesh> meshes;
		LoadedModel model;
		std::unique_ptr<AnimationDatabaseUpload> animation;
		std::size_t primitive = 0;
		std::uint64_t offset = 0;
		std::uint64_t ticket = 0;
		bool indices = false;
		bool finalised = false;
		bool complete = false;
		bool taken = false;

		~PreparedModelUpload()
		{
			if (!taken && manager)
			{
				manager->ReleaseModelTextures(model);
			}
		}
	};

	Expected<std::shared_ptr<AssetManager::PreparedModelUpload>> AssetManager::BeginPreparedModel(const assets::GltfAsset& source, std::string_view path)
	{
		try
		{
			auto upload = std::make_shared<PreparedModelUpload>();
			upload->manager = this;
			upload->source = &source;
			upload->path = path;
			upload->meshes.resize(source.primitives.size());
			for (std::size_t i = 0; i < source.primitives.size(); ++i)
			{
				const auto& primitive = source.primitives[i];
				if (primitive.vertices.empty())
					continue;
				upload->meshes[i] = Mesh::CreateUninitialized(static_cast<std::uint32_t>(primitive.vertices.size()), static_cast<std::uint32_t>(primitive.indices.size()), primitive.aabbMin, primitive.aabbMax, primitive.sphereCenter, primitive.sphereRadius);
				if (!upload->meshes[i].IsValid() || (!primitive.indices.empty() && !upload->meshes[i].IsIndexed()))
					return std::unexpected(AetherError::Asset("Failed to allocate streamed model mesh: " + upload->path));
			}
			return upload;
		}
		catch (const std::exception& error)
		{
			return std::unexpected(AetherError::Asset("Failed to prepare model '" + std::string(path) + "': " + error.what()));
		}
	}

	std::uint64_t AssetManager::PreparedModelLatestTicket(const PreparedModelUpload& upload) const
	{
		return upload.animation ? std::max(upload.ticket, upload.animation->LatestTicket()) : upload.ticket;
	}

	Expected<std::uint64_t> AssetManager::PreparedModelNextStagingRequirement(const PreparedModelUpload& upload, std::uint64_t budget) const
	{
		if (upload.complete || (upload.ticket && !m_uploadContext->IsUploadComplete(upload.ticket)))
			return 0;
		std::size_t primitiveIndex = upload.primitive;
		std::uint64_t offset = upload.offset;
		bool indices = upload.indices;
		while (primitiveIndex < upload.source->primitives.size())
		{
			const auto& primitive = upload.source->primitives[primitiveIndex];
			if (primitive.vertices.empty())
			{
				++primitiveIndex;
				continue;
			}
			const auto bytes = indices ? primitive.indices.size() * sizeof(std::uint32_t) : primitive.vertices.size() * sizeof(Mesh::Vertex);
			if (offset == bytes)
			{
				offset = 0;
				if (!indices && !primitive.indices.empty())
					indices = true;
				else
				{
					indices = false;
					++primitiveIndex;
				}
				continue;
			}
			const auto count = std::min<std::uint64_t>(bytes - offset, budget & ~std::uint64_t{3});
			if (!count)
				return 0;
			gpu::MappedBufferDesc desc{};
			desc.size = count;
			desc.usage = gpu::BufferUsage::TransferSrc;
			const auto requirement = gpu::ResourceRegistry::GetMappedBufferAllocationRequirement(desc);
			if (!requirement)
				return std::unexpected(AetherError::Engine("Failed to query streamed mesh staging requirement"));
			return requirement;
		}
		if (upload.animation)
			return upload.animation->NextStagingRequirement(static_cast<std::size_t>(budget));
		return 0;
	}

	Expected<AssetManager::ModelUploadProgress> AssetManager::PumpPreparedModel(PreparedModelUpload& upload, std::uint64_t budget, std::function<void()> onStagingRetired)
	{
		struct CallbackGuard
		{
			std::function<void()> callback;
			~CallbackGuard() { if (callback) callback(); }
		} retirement{std::move(onStagingRetired)};
		ModelUploadProgress progress{.ticket = upload.ticket, .complete = upload.complete};
		if (upload.complete || (upload.ticket && !m_uploadContext->IsUploadComplete(upload.ticket)))
			return progress;
		try
		{
			const auto& source = *upload.source;
			while (upload.primitive < source.primitives.size())
			{
				const auto& primitive = source.primitives[upload.primitive];
				if (primitive.vertices.empty())
				{
					++upload.primitive;
					continue;
				}
				const auto bytes = upload.indices ? primitive.indices.size() * sizeof(std::uint32_t) : primitive.vertices.size() * sizeof(Mesh::Vertex);
				if (upload.offset == bytes)
				{
					upload.offset = 0;
					if (!upload.indices && !primitive.indices.empty())
						upload.indices = true;
					else
					{
						upload.indices = false;
						++upload.primitive;
					}
					continue;
				}
				const auto count = std::min<std::uint64_t>(bytes - upload.offset, budget & ~std::uint64_t{3});
				if (!count)
					return progress;
				gpu::MappedBufferDesc desc{};
				desc.size = count;
				desc.usage = gpu::BufferUsage::TransferSrc;
				desc.debugName = "Mesh.StreamingStaging";
				const auto staging = gpu::ResourceRegistry::CreateMappedBuffer(desc);
				if (!staging.IsValid())
					return std::unexpected(AetherError::Asset("Failed to allocate streamed mesh staging"));
				const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(staging);
				if (!view.mappedPtr)
				{
					const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
					gpu::ResourceRegistry::Destroy(staging);
					if (retirement.callback)
						gpu::ResourceRegistry::DeferDestructionCallback(std::move(retirement.callback));
					return std::unexpected(AetherError::Asset("Failed to map streamed mesh staging"));
				}
				const auto* data = upload.indices ? reinterpret_cast<const std::byte*>(primitive.indices.data()) : reinterpret_cast<const std::byte*>(primitive.vertices.data());
				std::memcpy(view.mappedPtr, data + upload.offset, static_cast<std::size_t>(count));
				gpu::ResourceRegistry::FlushMappedBuffer(staging, 0, count);
				const auto destination = upload.indices ? upload.meshes[upload.primitive].GetIndexBuffer() : upload.meshes[upload.primitive].GetBuffer();
				const auto ticket = m_uploadContext->CopyBufferAsyncOwned(staging, destination, count, upload.offset, std::move(retirement.callback));
				if (!ticket)
					return std::unexpected(AetherError::Asset("Failed to submit streamed mesh upload"));
				upload.ticket = ticket;
				upload.offset += count;
				progress.bytes = count;
				progress.ticket = ticket;
				return progress;
			}
			if (!upload.finalised)
			{
				const std::vector<TextureHandle> handles;
				auto result = FinaliseModelLoad(upload.model, source, handles, upload.path, &upload.ticket, &upload.meshes);
				if (!result)
					return std::unexpected(result.error());
				upload.finalised = true;
				if (!source.animations.empty())
				{
					auto animation = AnimationDatabase::BeginUpload(*m_context, source);
					if (!animation)
						return std::unexpected(animation.error());
					upload.animation = std::move(*animation);
					// The caller must reserve the first animation chunk separately.
					return progress;
				}
			}
			if (upload.animation)
			{
				auto animation = upload.animation->Pump(static_cast<std::size_t>(budget), std::move(retirement.callback));
				if (!animation)
					return std::unexpected(animation.error());
				upload.ticket = std::max(upload.ticket, animation->ticket);
				progress.bytes = animation->bytes;
				progress.ticket = upload.ticket;
				if (!animation->complete)
					return progress;
				upload.model.animationDb = upload.animation->Take();
				upload.animation.reset();
			}
			upload.complete = true;
			progress.complete = true;
			return progress;
		}
		catch (const std::exception& error)
		{
			return std::unexpected(AetherError::Asset("Failed to upload model '" + upload.path + "': " + error.what()));
		}
	}

	LoadedModel AssetManager::TakePreparedModel(PreparedModelUpload& upload)
	{
		upload.taken = true;
		return std::move(upload.model);
	}


	Expected<void> AssetManager::FinaliseModelLoad(LoadedModel& loaded, const assets::GltfAsset& source, const std::vector<TextureHandle>& imageHandles, std::string_view path, std::uint64_t* uploadTicket, std::vector<Mesh>* preparedMeshes)
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

		std::string missingTexture;
		auto acquireTexture = [this, uploadTicket, &missingTexture](std::string_view texturePath, TextureColorSpace colorSpace) -> TextureHandle
		{
			if (texturePath.empty())
			{
				return {};
			}
			TextureHandle handle = uploadTicket ? m_textureRegistry->FindAndAcquire(texturePath, colorSpace) : m_textureRegistry->Acquire(texturePath, colorSpace);
			if (uploadTicket && !handle.IsValid() && missingTexture.empty())
				missingTexture = texturePath;
			return handle;
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
			struct PrimitiveFailureGuard
			{
				LoadedModelPrimitive& primitive;
				TextureRegistry& textures;
				gpu::UploadContext& uploads;
				std::uint64_t* ticket;
				int exceptions = std::uncaught_exceptions();
				~PrimitiveFailureGuard()
				{
					if (ticket && std::uncaught_exceptions() > exceptions)
					{
						uploads.WaitForUpload(*ticket);
						const auto& mat = primitive.material;
						for (TextureHandle handle : {mat.albedoTex, mat.normalTex, mat.metallicRoughnessTex, mat.occlusionTex, mat.emissiveTex})
							textures.Release(handle);
					}
				}
			} failureGuard{loadedPrim, *m_textureRegistry, *m_uploadContext, uploadTicket};
			if (preparedMeshes)
				loadedPrim.mesh = std::move((*preparedMeshes)[primIdx]);
			else if (uploadTicket)
			{
				std::uint64_t meshTicket = 0;
				loadedPrim.mesh = Mesh::Create(*m_uploadContext, primitive.vertices, primitive.indices, primitive.aabbMin, primitive.aabbMax, primitive.sphereCenter, primitive.sphereRadius, &meshTicket);
				*uploadTicket = std::max(*uploadTicket, meshTicket);
				if (!loadedPrim.mesh.IsValid() || (!primitive.indices.empty() && !loadedPrim.mesh.IsIndexed()))
				{
					m_uploadContext->WaitForUpload(*uploadTicket);
					return std::unexpected(AetherError::Asset("Failed to upload model mesh: " + std::string(path)));
				}
			}
			else
				loadedPrim.mesh = CreateMesh(primitive.vertices, primitive.indices, primitive.aabbMin, primitive.aabbMax, primitive.sphereCenter, primitive.sphereRadius);
			loadedPrim.primitiveKey = assets::GltfAsset::PrimitiveKey(primitive);
			loadedPrim.legacyGeometryKey = primitive.primitiveKey.empty() ? loadedPrim.primitiveKey : assets::GltfAsset::GeometryPrimitiveKey(primitive);
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
				mat.additiveBlend = srcMat.additiveBlend;
				mat.alphaMask = srcMat.alphaMask;
				mat.modulateVertexColor = srcMat.modulateVertexColor;
				mat.foliage = srcMat.foliage;
				mat.bakedLighting = srcMat.bakedLighting;
				mat.sky = srcMat.sky;
				mat.lightShaft = srcMat.lightShaft;
				mat.gem = srcMat.gem;
				mat.sheen = srcMat.sheen;
				mat.water = srcMat.water;
				mat.character = srcMat.character;
				mat.objectLit = srcMat.objectLit;
				mat.uvScroll = srcMat.uvScroll;

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
			failureGuard.ticket = nullptr;
			if (!missingTexture.empty())
				return std::unexpected(AetherError::Asset("Prepared model texture is not resident: " + missingTexture));
		}

		AE_VERBOSE(LogCategory::Engine, "Loaded model '{}': {} primitive(s), {} animation(s).", std::string(path), loaded.primitives.size(), source.animations.size());

		// A skin is NOT required: AnimationDatabase::Create only needs animations+nodes
		// (it gates on animations.empty() alone, tolerating skins=0 - see its own
		// comment). Requiring skins here used to silently drop every skinless-but-
		// animated model (a Kenney-style lever/valve/door - node-TRS animation, no
		// per-vertex skinning at all) with nothing louder than the "No animation
		// database" INFO line below; GltfAsset already resolved those channels'
		// baked bone-name tags against this model's own nodes at load time, so
		// source.animations[*].nodeIndex is correct here exactly like a skinned model.
		if (!source.animations.empty() && !preparedMeshes)
		{
			AE_VERBOSE(LogCategory::Engine, "  Creating animation database: {} skins, {} animations, {} nodes", source.skins.size(), source.animations.size(), source.nodes.size());
			loaded.animationDb = AnimationDatabase::Create(*m_context, source);
			if (uploadTicket)
				*uploadTicket = std::max(*uploadTicket, m_context->GetTransferManager().LastSubmitted());
			if (loaded.animationDb.IsValid())
			{
				AE_VERBOSE(LogCategory::Engine, "  Animation database created: {} clips, {} nodes, {} skins", loaded.animationDb.GetClipCount(), loaded.animationDb.GetNodeCount(), loaded.animationDb.GetSkinCount());
			}
			else
			{
				AE_WARN(LogCategory::Engine, "  Animation database creation returned empty (no clips?)");
				if (uploadTicket)
					return std::unexpected(AetherError::Asset("Failed to create model animation database: " + std::string(path)));
			}
		}
		else
		{
			AE_INFO(LogCategory::Engine, "  No animation database: skins={}, animations={}", source.skins.size(), source.animations.size());
		}
		return {};
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
