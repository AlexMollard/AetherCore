#include "SandboxLayer.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "Components.hpp"
#include "Logger.hpp"
#include "Material.hpp"
#include "MeowCore.hpp"
#include "World.hpp"

namespace meow::app
{
	void SandboxLayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "Sandbox layer attached.");

		// ── Pipeline with bindless layout ──────────────────────────────────
		const VkDescriptorSetLayout bindlessLayout =
			context.engine.GetBindlessManager().GetLayout();

		m_cubePipeline = context.engine.CreateGraphicsPipeline({
			.shaderVfsPath = "shaders://hellotriangle.slang.spv",
			.colorFormat   = context.engine.GetForwardColorFormat(),
			.depthFormat   = context.engine.GetSwapchainDepthFormat(),
			.depthTestEnable  = true,
			.depthWriteEnable = true,
			.setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
			});

		m_cubeMesh = &context.engine.GetPrimitiveMesh(meow::PrimitiveMesh::Cube);

		// ── Register two cube entities in the World ─────────────────────────
		m_cubeEntity = context.world->CreateEntity();
		context.world->Set(m_cubeEntity, PipelineComponent{ .pipeline  = &m_cubePipeline });
		context.world->Set(m_cubeEntity, MeshComponent    { .mesh      = m_cubeMesh });
		context.world->Set(m_cubeEntity, TransformComponent{});  // identity, updated every frame
		context.world->Set(m_cubeEntity, MaterialComponent{});   // no texture → vertex colour

		m_cubeEntityTwo = context.world->CreateEntity();
		context.world->Set(m_cubeEntityTwo, PipelineComponent  { .pipeline = &m_cubePipeline });
		context.world->Set(m_cubeEntityTwo, MeshComponent      { .mesh     = m_cubeMesh });
		context.world->Set(m_cubeEntityTwo, TransformComponent {});

		m_cubeTexture = context.engine.CreateTexture("assets://textures/tex_DebugUVTiles.png");
		context.world->Set(m_cubeEntityTwo, MaterialComponent{
			.material = { .albedoSlot = m_cubeTexture.GetBindlessSlot() }
		});

		INFO(LogCategory::App, "Sandbox cubes registered in world.");
	}

	void SandboxLayer::OnDetach(LayerContext& context)
	{
		context.world->DestroyEntity(m_cubeEntity);
		context.world->DestroyEntity(m_cubeEntityTwo);
		m_cubeTexture.Destroy();
		m_cubeMesh = nullptr;
		m_cubePipeline.Destroy();
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		// Rotate 45 degrees per second around Y.
		m_rotation += static_cast<float>(context.deltaTimeSeconds) * 45.0f;

		const glm::mat4 model = glm::rotate(
			glm::mat4{ 1.0f },
			glm::radians(m_rotation),
			{ 0.0f, 1.0f, 0.0f });
		context.world->Set(m_cubeEntity, TransformComponent{ .localToWorld = model });

		glm::mat4 modelTwo = glm::rotate(
			glm::mat4{ 1.0f },
			glm::radians(m_rotation),
			{ 0.0f, -1.0f, 0.0f });
		modelTwo = glm::translate(modelTwo, { 0.0f, 0.0f, -2.0f });
		context.world->Set(m_cubeEntityTwo, TransformComponent{ .localToWorld = modelTwo });

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
	{
		(void)context;
	}
}
