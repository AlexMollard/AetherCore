#include "SandboxLayer.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "Logger.hpp"
#include "MeowCore.hpp"

namespace meow::app
{
	void SandboxLayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "Sandbox layer attached.");

		m_cubePipeline = context.engine.CreateGraphicsPipeline({
			.shaderVfsPath = "shaders://hellotriangle.slang.spv",
			.colorFormat = context.engine.GetSwapchainImageFormat(),
			.depthFormat = context.engine.GetSwapchainDepthFormat(),
			.depthTestEnable = true,
			.depthWriteEnable = true,
			});

		m_cubeMesh = &context.engine.GetPrimitiveMesh(meow::PrimitiveMesh::Cube);

		m_cubeHandle = context.scene->AddRenderObject({
			.pipeline = &m_cubePipeline,
			.mesh = m_cubeMesh,
			});

		m_cubeHandleTwo = context.scene->AddRenderObject({
			.pipeline = &m_cubePipeline,
			.mesh = m_cubeMesh,
			});

		// Set up a perspective camera looking at the origin.
		// The VP is recomputed every frame in OnUpdate to handle window resizes.
		INFO(LogCategory::App, "Hello-triangle registered in scene.");
	}

	void SandboxLayer::OnDetach(LayerContext& context)
	{
		context.scene->RemoveRenderObject(m_cubeHandle);
		context.scene->RemoveRenderObject(m_cubeHandleTwo);
		m_cubeMesh = nullptr;
		m_cubePipeline.Destroy();
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		// Rotate 45 degrees per second around Y.
		m_rotation += static_cast<float>(context.deltaTimeSeconds) * 45.0f;

		glm::mat4 model = glm::rotate(
			glm::mat4{ 1.0f },
			glm::radians(m_rotation),
			{ 0.0f, 1.0f, 0.0f });
		context.scene->SetTransform(m_cubeHandle, model);

		glm::mat4 modelTwo = glm::rotate(
			glm::mat4{ 1.0f },
			glm::radians(m_rotation),
			{ 0.0f, -1.0f, 0.0f });
		
		modelTwo = glm::translate(modelTwo, { 0.0f, 0.0f, -2.0f });
		context.scene->SetTransform(m_cubeHandleTwo, modelTwo);

		// Recompute the view-projection (accounts for window resize).
		const VkExtent2D extent = context.engine.GetSwapchainExtent();
		const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

		glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
		proj[1][1] *= -1.0f;  // Flip Y: GLM uses OpenGL convention, Vulkan has Y pointing down.

		const glm::mat4 view = glm::lookAt(
			glm::vec3{ 0.0f, 1.5f, 3.0f },   // camera position
			glm::vec3{ 0.0f, 0.0f, 0.0f },   // look-at target
			glm::vec3{ 0.0f, 1.0f, 0.0f });  // up vector

		context.scene->SetViewProjection(proj * view);
	}

	void SandboxLayer::OnGui(LayerContext& context)
	{}
}
