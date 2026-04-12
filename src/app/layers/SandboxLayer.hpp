#pragma once

#include "AppLayer.hpp"
#include "Entity.hpp"
#include "GraphicsPipeline.hpp"
#include "Texture.hpp"

namespace meow
{
	class Mesh;
}

namespace meow::app
{
	class SandboxLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		meow::GraphicsPipeline m_cubePipeline;
		const meow::Mesh*      m_cubeMesh = nullptr;
		meow::Entity           m_cubeEntity;
		meow::Entity           m_cubeEntityTwo;
		meow::Texture          m_cubeTexture;
		float                  m_rotation = 0.0f;  // degrees, accumulated over time
	};
}