#pragma once

#include <cstdint>

#include "gpu/UploadContext.hpp"
#include "mesh/MeshArena.hpp"
#include "mesh/MeshUploadQueue.hpp"
#include "material/MaterialBuffer.hpp"
#include "material/AssetTextureSink.hpp"
#include "material/TextureRegistry.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialAuthoring.hpp"
#include "material/PipelineCache.hpp"
#include "material/EffectParamBuffer.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "assets/AssetManager.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	class RenderQueue;
	class ShadowService;
	class RenderTargetService;
	class VulkanContext;
	class World;

	// Owns the asset loading and GPU resource creation services.
	class AssetSubsystem
	{
	public:
		void Init(ServiceContainer& services);
		void Shutdown();

		// Called after RenderingSubsystem init to wire up rendering deps
		// for the AssetManager (needed for animation database registration).
		void LinkRenderingDeps(ServiceContainer& services);

		[[nodiscard]] AssetManager& GetAssetManager()
		{
			return m_assetManager;
		}

		[[nodiscard]] MeshArena& GetMeshArena()
		{
			return m_meshArena;
		}

		[[nodiscard]] MeshUploadQueue& GetMeshUploadQueue()
		{
			return m_meshUploadQueue;
		}

		[[nodiscard]] MaterialBuffer& GetMaterialBuffer()
		{
			return m_materialBuffer;
		}

		[[nodiscard]] MaterialRegistry& GetMaterialRegistry()
		{
			return m_materialRegistry;
		}

		[[nodiscard]] TextureRegistry& GetTextureRegistry()
		{
			return m_textureRegistry;
		}

		[[nodiscard]] MaterialAuthoring& GetMaterialAuthoring()
		{
			return m_materialAuthoring;
		}

		[[nodiscard]] PipelineCache& GetPipelineCache()
		{
			return m_pipelineCache;
		}

		[[nodiscard]] EffectParamBuffer& GetEffectParamBuffer()
		{
			return m_effectParamBuffer;
		}

		// Initialize the pipeline cache's frame-graph-constant Context + factory.
		// Called by the app layer once color/depth formats + heap mappings are known.
		void InitializePipelineCache(PipelineCache::Context context);

		[[nodiscard]] PrimitiveMeshes& GetPrimitiveMeshes()
		{
			return m_primitiveMeshes;
		}

		[[nodiscard]] gpu::UploadContext& GetUploadContext()
		{
			return m_uploadContext;
		}

		// Flush pending mesh uploads via a one-shot command buffer.
		void FlushMeshUploads();

		// Advance per-frame GPU bookkeeping (material slot deferred-free clock).
		// Called once per frame from the render loop.
		void AdvanceFrame(std::uint64_t frameIndex);

	private:
		AssetManager m_assetManager;
		MeshArena m_meshArena;
		MeshUploadQueue m_meshUploadQueue;
		MaterialBuffer m_materialBuffer;
		// Declared before m_materialRegistry: PackMaterial + the material->texture
		// cascade reference the TextureRegistry. Sink declared before the registry.
		AssetTextureSink m_textureSink;
		TextureRegistry m_textureRegistry{m_textureSink};
		// Declared after m_materialBuffer + m_textureRegistry: the registry's sink
		// reference binds to a constructed buffer and it forwards the texture registry.
		MaterialRegistry m_materialRegistry{m_materialBuffer, m_textureRegistry};
		PipelineCache m_pipelineCache;
		EffectParamBuffer m_effectParamBuffer;
		// Declared after the registry + cache: the authoring layer references both.
		MaterialAuthoring m_materialAuthoring{m_materialRegistry, m_pipelineCache};
		PrimitiveMeshes m_primitiveMeshes;
		gpu::UploadContext m_uploadContext;
		VulkanContext* m_context = nullptr;
		World* m_world = nullptr;
	};
} // namespace aether
