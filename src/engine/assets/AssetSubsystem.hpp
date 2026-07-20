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
#include "assets/AssetDatabase.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/TileAssetStore.hpp"
#include "rendering/TileMapSystem.hpp"
#include "rendering/SpriteSystem.hpp"

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

	class AssetSubsystem
	{
	public:
		void Init(ServiceContainer& services);
		void Shutdown();

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

		void InitializePipelineCache(PipelineCache::Context context);

		[[nodiscard]] PrimitiveMeshes& GetPrimitiveMeshes()
		{
			return m_primitiveMeshes;
		}

		[[nodiscard]] AssetDatabase& GetAssetDatabase()
		{
			return m_assetDatabase;
		}

		[[nodiscard]] gpu::UploadContext& GetUploadContext()
		{
			return m_uploadContext;
		}

		[[nodiscard]] TileMapSystem& GetTileMapSystem()
		{
			return m_tileMapSystem;
		}

		[[nodiscard]] SpriteSystem& GetSpriteSystem()
		{
			return m_spriteSystem;
		}

		[[nodiscard]] TileAssetStore& GetTileAssetStore()
		{
			return m_tileAssetStore;
		}

		[[nodiscard]] SpriteAssetStore& GetSpriteAssetStore()
		{
			return m_spriteAssetStore;
		}

		void FlushMeshUploads();

		void AdvanceFrame(std::uint64_t frameIndex);

	private:
		AssetManager m_assetManager;
		MeshArena m_meshArena;
		MeshUploadQueue m_meshUploadQueue;
		MaterialBuffer m_materialBuffer;
		AssetTextureSink m_textureSink;
		TextureRegistry m_textureRegistry{m_textureSink};
		MaterialRegistry m_materialRegistry{m_materialBuffer, m_textureRegistry};
		PipelineCache m_pipelineCache;
		EffectParamBuffer m_effectParamBuffer;
		MaterialAuthoring m_materialAuthoring{m_materialRegistry, m_pipelineCache};
		PrimitiveMeshes m_primitiveMeshes;
		AssetDatabase m_assetDatabase{m_primitiveMeshes};
		gpu::UploadContext m_uploadContext;
		SpriteAssetStore m_spriteAssetStore;
		TileAssetStore m_tileAssetStore;
		SpriteSystem m_spriteSystem;
		TileMapSystem m_tileMapSystem;
		VulkanContext* m_context = nullptr;
		World* m_world = nullptr;
	};
} // namespace aether
