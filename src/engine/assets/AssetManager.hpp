#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "rendering/GraphicsPipeline.hpp"
#include "mesh/Mesh.hpp"
#include "material/Texture.hpp"
#include "material/TextureHandle.hpp"
#include "utils/coro/Task.hpp"
#include "gpu/UploadContext.hpp"

namespace aether
{
	struct MaterialAsset;
	class MaterialRegistry;
	class MaterialAuthoring;
	class PipelineCache;
	class EffectParamBuffer;
	class TextureRegistry;
	class VulkanContext;

	namespace assets
	{
		struct GltfAsset;
	}
	class BindlessManager;
	class RenderQueue;
	class ShadowService;
	class RenderTargetService;
	class World;
	struct LoadedModel;
	struct LoadedModelPrimitive;
	struct Entity;

	// Asset manager service - owns GPU resource creation and loading.
	class AssetManager
	{
	public:
		AssetManager() = default;
		~AssetManager() = default;

		// Mesh creation.
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices, const float* aabbMin, const float* aabbMax, const float* sphereCenter, float sphereRadius);

		// Texture creation.
		[[nodiscard]] Expected<Texture> CreateTexture(std::string_view path);

		// Async texture creation - co_await the file read on the I/O thread,
		// then decode and upload to GPU on the calling (game) thread.
		[[nodiscard]] coro::async<Expected<Texture>> CreateTextureAsync(std::string_view path);

		// Pipeline creation.
		[[nodiscard]] Expected<GraphicsPipeline> CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);

		// Load a material preset file. Returns pure authoring data; referenced
		// textures are acquired (ref-counted) from the TextureRegistry and held by
		// the returned MaterialAsset's TextureHandles.
		[[nodiscard]] Expected<MaterialAsset> LoadMaterialPreset(std::string_view path);

		// Release the texture handles a LoadedModel's primitives hold (the loader
		// refs taken in FinaliseModelLoad). Call before discarding a cached model
		// so its textures free once no spawned entity references them anymore.
		void ReleaseModelTextures(LoadedModel& model);

		// Model loading and spawning.
		[[nodiscard]] Expected<LoadedModel> LoadModel(std::string_view path);

		// Async model loading - co_await the .mesh file read on the I/O thread,
		// then parse and upload textures on the game thread.
		[[nodiscard]] coro::async<Expected<LoadedModel>> LoadModelAsync(std::string_view path);

		[[nodiscard]] std::vector<Entity> SpawnModel(LoadedModel& model, std::uint32_t parentEntityId = 0, float scale = 1.0f);

		// Bind runtime dependencies once during engine startup.
		void Initialize(VulkanContext& context,
		        BindlessManager& bindlessManager,
		        MaterialRegistry& materialRegistry,
		        MaterialAuthoring& materialAuthoring,
		        PipelineCache& pipelineCache,
		        EffectParamBuffer& effectParamBuffer,
		        TextureRegistry& textureRegistry,
		        World& world,
		        gpu::UploadContext& uploadContext);

		[[nodiscard]] MaterialRegistry& GetMaterialRegistry()
		{
			return *m_materialRegistry;
		}

		[[nodiscard]] MaterialAuthoring& GetMaterialAuthoring()
		{
			return *m_materialAuthoring;
		}

		[[nodiscard]] PipelineCache& GetPipelineCache()
		{
			return *m_pipelineCache;
		}

		[[nodiscard]] EffectParamBuffer& GetEffectParamBuffer()
		{
			return *m_effectParamBuffer;
		}

		[[nodiscard]] TextureRegistry& GetTextureRegistry()
		{
			return *m_textureRegistry;
		}

		// Shared finalisation step for both synchronous and async model loading.
		void FinaliseModelLoad(LoadedModel& loaded, const assets::GltfAsset& source, const std::vector<TextureHandle>& imageHandles, std::string_view path);

		// Set rendering dependencies after the rendering subsystem initializes.
		void SetRenderQueue(RenderQueue& renderQueue)
		{
			m_renderQueue = &renderQueue;
		}

		void SetShadowService(ShadowService& shadowService)
		{
			m_shadowService = &shadowService;
		}

		void SetRenderTargetService(RenderTargetService& renderTargetService)
		{
			m_renderTargetService = &renderTargetService;
		}

	private:
		VulkanContext* m_context = nullptr;
		BindlessManager* m_bindlessManager = nullptr;
		MaterialRegistry* m_materialRegistry = nullptr;
		MaterialAuthoring* m_materialAuthoring = nullptr;
		PipelineCache* m_pipelineCache = nullptr;
		EffectParamBuffer* m_effectParamBuffer = nullptr;
		TextureRegistry* m_textureRegistry = nullptr;
		RenderQueue* m_renderQueue = nullptr;
		ShadowService* m_shadowService = nullptr;
		RenderTargetService* m_renderTargetService = nullptr;
		World* m_world = nullptr;
		gpu::UploadContext* m_uploadContext = nullptr;

		// Deduplicated, AssetManager-lifetime storage for per-material shader VFS
		// path overrides parsed from a material preset (binary .material or
		// properties.toml fallback). MaterialTemplate::shaderVfsPath is a
		// string_view that PipelineCache stores by value for the cache's lifetime
		// (see MaterialTemplate.hpp), so a locally-parsed std::string cannot back
		// it directly -- it must be interned somewhere that outlives the cache.
		// unordered_set gives both dedup (repeated loads of the same override
		// don't grow this unboundedly) and reference stability across inserts.
		std::unordered_set<std::string> m_internedShaderVfsPaths;

		// Intern a per-material shader VFS path so a stable string_view can be
		// handed to MaterialTemplate::shaderVfsPath. Returns an empty view for an
		// empty input (caller then keeps whatever default the MaterialAsset
		// already carries).
		[[nodiscard]] std::string_view InternShaderVfsPath(std::string path);
	};
} // namespace aether
