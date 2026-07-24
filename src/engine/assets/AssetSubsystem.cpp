#include "assets/AssetSubsystem.hpp"

#include <stdexcept>

#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "material/EffectSystem.hpp"
#include "material/Texture.hpp"
#include "ui/UiImageSystem.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "rendering/ShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "scene/World.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/ResourceRegistry.hpp"

namespace aether
{
	void AssetSubsystem::Init(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		services.Register<PrimitiveMeshes>(m_primitiveMeshes);

		m_context = &services.Get<VulkanContext>();
		VulkanContext& vk = *m_context;
		auto& bindless = services.Get<BindlessManager>();
		auto& world = services.Get<World>();

		auto& registry = services.Get<aether::ResourceRegistry>();
		m_uploadContext = gpu::UploadContext::Create(static_cast<void*>(vk.GetDevice().device), vk.GetGraphicsQueueFamily(), static_cast<void*>(vk.GetGraphicsQueue()), static_cast<void*>(&registry));

		m_materialBuffer.Initialize();
		m_effectParamBuffer.Initialize();
		m_textureSink.Initialize(vk, m_uploadContext, bindless.GetCapacity());
		// error texture can never itself fail to load.
		AE_EXPECT_OR_THROW(magentaFallback, Texture::CreateSolidColor({255, 0, 255, 255}, vk.GetDevice().device, vk.GetGraphicsQueue(), m_uploadContext.GetCommandPool()));
		m_textureRegistry.InitializeDefault(TextureResource{std::move(magentaFallback)});
		// Solid white: what untextured sprites sample, so their tint shows true
		// (magenta stays reserved for "asset load failed").
		AE_EXPECT_OR_THROW(whiteBuiltin, Texture::CreateSolidColor({255, 255, 255, 255}, vk.GetDevice().device, vk.GetGraphicsQueue(), m_uploadContext.GetCommandPool()));
		m_textureRegistry.InitializeWhite(TextureResource{std::move(whiteBuiltin)});
		m_spriteSystem.Initialize(m_textureRegistry, m_spriteAssetStore);
		m_tileMapSystem.Initialize(m_textureRegistry, m_spriteAssetStore, m_tileAssetStore);
		services.Register<SpriteAssetStore>(m_spriteAssetStore);
		services.Register<TileAssetStore>(m_tileAssetStore);

		MaterialAsset defaultAsset;
		defaultAsset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
		defaultAsset.roughnessFactor = 0.6f;
		defaultAsset.metallicFactor = 0.0f;
		m_materialRegistry.InitializeDefault(defaultAsset);

		m_primitiveMeshes.Initialize(m_uploadContext);
		services.Register<AssetDatabase>(m_assetDatabase);
		m_assetDatabase.RegisterBuiltinPrimitives();
		m_world = &world;
		m_assetManager.Initialize(vk, bindless, m_materialRegistry, m_materialAuthoring, m_pipelineCache, m_effectParamBuffer, m_textureRegistry, world, m_uploadContext);
		EffectSystem::ConnectLifecycle(world, m_effectParamBuffer);
		UiImageSystem::ConnectLifecycle(world, m_textureRegistry);
	}

	void AssetSubsystem::InitializePipelineCache(PipelineCache::Context context)
	{
		m_pipelineCache.Initialize(context, [this](const GraphicsPipeline::Desc& desc) { return m_assetManager.CreateGraphicsPipeline(desc); });
	}

	void AssetSubsystem::LinkRenderingDeps(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		m_assetManager.SetRenderQueue(services.Get<RenderQueue>());
		m_assetManager.SetShadowService(services.Get<ShadowService>());
		m_assetManager.SetRenderTargetService(services.Get<RenderTargetService>());
	}

	void AssetSubsystem::AdvanceFrame(std::uint64_t frameIndex)
	{
		m_materialBuffer.AdvanceFrame(frameIndex);
		m_effectParamBuffer.AdvanceFrame(frameIndex);
	}


	void AssetSubsystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_world != nullptr)
		{
			MaterialSystem::DisconnectLifecycle(*m_world);
			EffectSystem::DisconnectLifecycle(*m_world);
			UiImageSystem::DisconnectLifecycle(*m_world);
			m_world = nullptr;
		}
		m_materialAuthoring.ReleaseAll();
		m_tileMapSystem.Shutdown();
		m_spriteSystem.Shutdown();
		m_spriteAssetStore.Clear();
		m_textureRegistry.ReleaseAll();
		m_assetManager = AssetManager{};
		m_primitiveMeshes.Destroy();
		m_materialBuffer.Shutdown();
		m_effectParamBuffer.Shutdown();
		// The cache borrows BindlessManager's heap mappings; it must shut down
		m_pipelineCache.Shutdown();

		if (m_uploadContext.IsValid())
		{
			m_uploadContext.Destroy();
		}
	}
} // namespace aether
