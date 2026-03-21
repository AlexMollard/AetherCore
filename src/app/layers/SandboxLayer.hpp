#pragma once

#include "AppLayer.hpp"
#include "GraphicsPipeline.hpp"
#include "Scene.hpp"

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
		meow::GraphicsPipeline m_trianglePipeline;
		const meow::Mesh* m_triangleMesh = nullptr;
		meow::Scene::Handle    m_triangleHandle;
	};
}