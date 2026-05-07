#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "GraphicsPipeline.hpp"
#include "Mesh.hpp"
#include "Texture.hpp"

namespace aether
{
	struct Material;
	class VulkanContext;
	class BindlessManager;
	class MaterialBuffer;
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

		// Texture creation.
		[[nodiscard]] Texture CreateTexture(std::string_view path, TextureFilter filter = TextureFilter::Linear);

		// Pipeline creation.
		[[nodiscard]] GraphicsPipeline CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);

		// Material management.
		void RegisterMaterial(Material& mat);
		void UnregisterMaterial(Material& mat);

		// Load a material preset file and keep referenced textures alive in
		// outTextures. The returned material is already registered on GPU.
		[[nodiscard]] Material LoadMaterialPreset(std::string_view path, std::vector<Texture>& outTextures);

		// Model loading and spawning.
		[[nodiscard]] LoadedModel LoadModel(std::string_view path);
		[[nodiscard]] std::vector<Entity> SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, float scale = 1.0f);

	private:
		friend class AetherCore; // Only AetherCore initializes/owns the AssetManager.

		// Bind runtime dependencies once during engine startup.
		void Initialize(VulkanContext& context, BindlessManager& bindlessManager, MaterialBuffer& materialBuffer, RenderQueue& renderQueue, ShadowService& shadowService, RenderTargetService& renderTargetService, World& world, VkCommandPool uploadPool);

		VulkanContext* m_context = nullptr;
		BindlessManager* m_bindlessManager = nullptr;
		MaterialBuffer* m_materialBuffer = nullptr;
		RenderQueue* m_renderQueue = nullptr;
		ShadowService* m_shadowService = nullptr;
		RenderTargetService* m_renderTargetService = nullptr;
		World* m_world = nullptr;
		VkCommandPool m_uploadPool = VK_NULL_HANDLE;
	};
} // namespace aether
