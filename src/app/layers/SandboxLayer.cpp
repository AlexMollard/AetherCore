#include "SandboxLayer.hpp"

#include "Logger.hpp"
#include "MeowCore.hpp"

namespace meow::app
{
	void SandboxLayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "Sandbox layer attached.");

		m_trianglePipeline = context.engine.CreateGraphicsPipeline({
			.shaderVfsPath = "shaders://hellotriangle.slang.spv",
			.colorFormat = context.engine.GetSwapchainImageFormat(),
			});

		m_triangleMesh = &context.engine.GetPrimitiveMesh(meow::PrimitiveMesh::Quad);

		m_triangleHandle = context.scene->AddRenderObject({
			.pipeline = &m_trianglePipeline,
			.mesh = m_triangleMesh,
			});

		INFO(LogCategory::App, "Hello-triangle registered in scene.");
	}

	void SandboxLayer::OnDetach(LayerContext& context)
	{
		context.scene->RemoveRenderObject(m_triangleHandle);
		m_triangleMesh = nullptr;
		m_trianglePipeline.Destroy();
	}

	void SandboxLayer::OnUpdate(LayerContext& /*context*/)
	{
	}

	void SandboxLayer::OnGui(LayerContext& context)
	{
	}
}
