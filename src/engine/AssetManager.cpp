#include "AssetManager.hpp"

#include "AetherCore.hpp"

namespace aether
{
	void AssetManager::Initialize(AetherCore* engine)
	{
		m_engine = engine;
	}

	Mesh AssetManager::CreateMesh(std::span<const Mesh::Vertex> vertices)
	{
		return m_engine->CreateMesh(vertices);
	}

	Mesh AssetManager::CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices)
	{
		return m_engine->CreateMesh(vertices, indices);
	}

	Texture AssetManager::CreateTexture(std::string_view path)
	{
		return m_engine->CreateTexture(path);
	}

	GraphicsPipeline AssetManager::CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc)
	{
		return m_engine->CreateGraphicsPipeline(desc);
	}

	void AssetManager::RegisterMaterial(Material& mat)
	{
		m_engine->RegisterMaterial(mat);
	}

	void AssetManager::UnregisterMaterial(Material& mat)
	{
		m_engine->UnregisterMaterial(mat);
	}

	LoadedModel AssetManager::LoadModel(std::string_view path)
	{
		return m_engine->LoadModel(path);
	}

	std::vector<Entity> AssetManager::SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, float scale)
	{
		return m_engine->SpawnModel(model, pipeline, scale);
	}
} // namespace aether
