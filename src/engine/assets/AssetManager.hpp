#pragma once

#include <cstdint>
#include <filesystem>
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

	class AssetManager
	{
	public:
		AssetManager() = default;
		~AssetManager() = default;

		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices, const float* aabbMin, const float* aabbMax, const float* sphereCenter, float sphereRadius);

		[[nodiscard]] Expected<Texture> CreateTexture(std::string_view path);

		[[nodiscard]] Expected<Texture> CreateTextureFromDisk(const std::filesystem::path& path);

		// Async texture creation - co_await the file read on the I/O thread,
		[[nodiscard]] coro::async<Expected<Texture>> CreateTextureAsync(std::string_view path);

		[[nodiscard]] Expected<GraphicsPipeline> CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);
		// The expensive half of the above, callable from any thread. See PipelineCache.
		[[nodiscard]] gpu::ResourceRegistry::PreparedPipeline PrepareGraphicsPipeline(const GraphicsPipeline::Desc& desc);

		[[nodiscard]] Expected<MaterialAsset> LoadMaterialPreset(std::string_view path);

		// MaterialTemplate holds its shader path as a string_view that the pipeline cache
		// outlives, so anyone building a material in memory - the material preview, and
		// anything that generates a shader - needs the same interning LoadMaterialPreset uses
		// rather than a second copy of that storage.
		[[nodiscard]] std::string_view InternShaderVfsPath(std::string path);

		void ReleaseModelTextures(LoadedModel& model);

		[[nodiscard]] Expected<LoadedModel> LoadModel(std::string_view path);

		// Async model loading - co_await the .mesh file read on the I/O thread,
		[[nodiscard]] coro::async<Expected<LoadedModel>> LoadModelAsync(std::string_view path);

		[[nodiscard]] std::vector<Entity> SpawnModel(LoadedModel& model, std::uint32_t parentEntityId = 0, float scale = 1.0f);

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

		void FinaliseModelLoad(LoadedModel& loaded, const assets::GltfAsset& source, const std::vector<TextureHandle>& imageHandles, std::string_view path);

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
		std::unordered_set<std::string> m_internedShaderVfsPaths;
	};
} // namespace aether
