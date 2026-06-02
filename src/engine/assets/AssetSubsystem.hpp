#pragma once

#include <cstdint>
#include "vulkan/volk.hpp"

#include "mesh/MeshArena.hpp"
#include "mesh/MeshUploadQueue.hpp"
#include "material/MaterialBuffer.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "assets/AssetManager.hpp"

namespace aether { class ServiceContainer; }

namespace aether
{
	class RenderQueue;
	class ShadowService;
	class RenderTargetService;
	class VulkanContext;

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

		[[nodiscard]] PrimitiveMeshes& GetPrimitiveMeshes()
		{
			return m_primitiveMeshes;
		}

		[[nodiscard]] VkCommandPool GetUploadPool() const
		{
			return m_uploadPool;
		}

		// Flush pending mesh uploads via a one-shot command buffer.
		void FlushMeshUploads();

	private:
		AssetManager m_assetManager;
		MeshArena m_meshArena;
		MeshUploadQueue m_meshUploadQueue;
		MaterialBuffer m_materialBuffer;
		PrimitiveMeshes m_primitiveMeshes;
		VkCommandPool m_uploadPool = VK_NULL_HANDLE;
		VulkanContext* m_context = nullptr;
	};
} // namespace aether
