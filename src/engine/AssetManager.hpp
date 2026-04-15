#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "Mesh.hpp"
#include "Texture.hpp"
#include "GraphicsPipeline.hpp"

namespace aether
{
	struct Material;
	class World;
	struct LoadedModel;
	struct LoadedModelPrimitive;
	struct Entity;

	// Asset manager service — owns GPU resource creation and loading.
	class AssetManager
	{
	public:
		AssetManager() = default;
		~AssetManager() = default;

		// Mesh creation.
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices,
			std::span<const std::uint32_t> indices);

		// Texture creation.
		[[nodiscard]] Texture CreateTexture(std::string_view path);

		// Pipeline creation.
		[[nodiscard]] GraphicsPipeline CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);

		// Material management.
		void RegisterMaterial(Material& mat);
		void UnregisterMaterial(Material& mat);

		// Model loading and spawning.
		[[nodiscard]] LoadedModel LoadModel(std::string_view path);
		[[nodiscard]] std::vector<Entity> SpawnModel(LoadedModel& model,
			GraphicsPipeline& pipeline,
			float scale = 1.0f);

	private:
		friend class AetherCore; // Only AetherCore initializes/owns the AssetManager

		// Initialize with dependencies (called by AetherCore).
		void Initialize(class AetherCore* engine);

		AetherCore* m_engine = nullptr;
	};
}
