#include "SandboxLayer.hpp"

#include <cmath>

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

		// ── Shared pipeline ───────────────────────────────────────────────────
		const VkDescriptorSetLayout bindlessLayout =
			context.engine.GetBindlessManager().GetLayout();

		m_pipeline = context.engine.CreateGraphicsPipeline({
			.shaderVfsPath    = "shaders://hellotriangle.slang.spv",
			.colorFormat      = context.engine.GetForwardColorFormat(),
			.depthFormat      = context.engine.GetSwapchainDepthFormat(),
			.depthTestEnable  = true,
			.depthWriteEnable = true,
			.setLayouts       = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
		});

		m_cubeMesh = &context.engine.GetPrimitiveMesh(meow::PrimitiveMesh::Cube);
		m_quadMesh = &context.engine.GetPrimitiveMesh(meow::PrimitiveMesh::Quad);

		m_debugTexture = context.engine.CreateTexture("assets://textures/tex_DebugUVTiles.png");
		const uint32_t texSlot = m_debugTexture.GetBindlessSlot();

		// Convenience lambdas to keep entity setup readable.
		auto pipe    = [&]() { return PipelineComponent{ .pipeline = &m_pipeline }; };
		auto withTex = [&]() { return MaterialComponent{ .material = { .albedoSlot = texSlot } }; };
		auto noTex   = []()  { return MaterialComponent{}; };

		// ── Ground quad (textured) ────────────────────────────────────────────
		m_groundEntity = context.world->CreateEntity();
		context.world->Set(m_groundEntity, pipe());
		context.world->Set(m_groundEntity, MeshComponent{ .mesh = m_quadMesh });
		context.world->Set(m_groundEntity, TransformComponent{});
		context.world->Set(m_groundEntity, withTex());

		// ── Centre cube (textured, multi-axis spin) ───────────────────────────
		m_centerEntity = context.world->CreateEntity();
		context.world->Set(m_centerEntity, pipe());
		context.world->Set(m_centerEntity, MeshComponent{ .mesh = m_cubeMesh });
		context.world->Set(m_centerEntity, TransformComponent{});
		context.world->Set(m_centerEntity, withTex());

		// ── Ring of 8 small cubes (vertex colour, no texture) ─────────────────
		for (int i = 0; i < kRingCount; ++i)
		{
			m_ringEntities[i] = context.world->CreateEntity();
			context.world->Set(m_ringEntities[i], pipe());
			context.world->Set(m_ringEntities[i], MeshComponent{ .mesh = m_cubeMesh });
			context.world->Set(m_ringEntities[i], TransformComponent{});
			context.world->Set(m_ringEntities[i], noTex());
		}

		// ── Two wider-orbit cubes (one textured, one vertex colour) ───────────
		m_orbitEntityA = context.world->CreateEntity();
		context.world->Set(m_orbitEntityA, pipe());
		context.world->Set(m_orbitEntityA, MeshComponent{ .mesh = m_cubeMesh });
		context.world->Set(m_orbitEntityA, TransformComponent{});
		context.world->Set(m_orbitEntityA, withTex());

		m_orbitEntityB = context.world->CreateEntity();
		context.world->Set(m_orbitEntityB, pipe());
		context.world->Set(m_orbitEntityB, MeshComponent{ .mesh = m_cubeMesh });
		context.world->Set(m_orbitEntityB, TransformComponent{});
		context.world->Set(m_orbitEntityB, noTex());

		INFO(LogCategory::App, "Scene built: ground + centre + {} ring + 2 orbit cubes.", kRingCount);
	}

	void SandboxLayer::OnDetach(LayerContext& context)
	{
		context.world->DestroyEntity(m_groundEntity);
		context.world->DestroyEntity(m_centerEntity);
		context.world->DestroyEntity(m_orbitEntityA);
		context.world->DestroyEntity(m_orbitEntityB);
		for (auto& e : m_ringEntities)
			context.world->DestroyEntity(e);

		m_debugTexture.Destroy();
		m_cubeMesh = nullptr;
		m_quadMesh = nullptr;
		m_pipeline.Destroy();
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		m_time        += static_cast<float>(context.deltaTimeSeconds);
		m_cameraAngle += static_cast<float>(context.deltaTimeSeconds) * 12.0f;  // 12 deg/s orbit

		const float t = m_time;

		// ── Ground: flat quad, 10×10, lying on Y = -0.5 ──────────────────────
		{
			glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, { 0.0f, -0.5f, 0.0f });
			m = glm::rotate(m, glm::radians(-90.0f), { 1.0f, 0.0f, 0.0f });
			m = glm::scale(m, { 10.0f, 10.0f, 1.0f });
			context.world->Set(m_groundEntity, TransformComponent{ .localToWorld = m });
		}

		// ── Centre cube: slow dual-axis spin, slightly scaled up ──────────────
		{
			glm::mat4 m = glm::rotate(glm::mat4{ 1.0f }, t * glm::radians(20.0f), { 0.0f, 1.0f, 0.0f });
			m = glm::rotate(m, t * glm::radians(9.0f), { 1.0f, 0.0f, 0.0f });
			m = glm::scale(m, { 1.2f, 1.2f, 1.2f });
			context.world->Set(m_centerEntity, TransformComponent{ .localToWorld = m });
		}

		// ── Ring: 8 cubes revolving around the origin ─────────────────────────
		constexpr float kRingRadius = 2.8f;
		for (int i = 0; i < kRingCount; ++i)
		{
			const float step     = glm::radians(360.0f / kRingCount);
			const float angle    = t * glm::radians(40.0f) + static_cast<float>(i) * step;
			const glm::vec3 pos  = { kRingRadius * std::cos(angle), 0.0f, kRingRadius * std::sin(angle) };
			const float selfSpin = t * glm::radians(90.0f + static_cast<float>(i) * 15.0f);

			glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, pos);
			m = glm::rotate(m, selfSpin, { 0.0f, 1.0f, 0.0f });
			m = glm::scale(m, { 0.35f, 0.35f, 0.35f });
			context.world->Set(m_ringEntities[i], TransformComponent{ .localToWorld = m });
		}

		// ── Orbit pair: 180° apart, wider radius, gentle Y bob ────────────────
		const glm::vec3 diagAxis = glm::normalize(glm::vec3{ 1.0f, 1.0f, 0.3f });
		for (int i = 0; i < 2; ++i)
		{
			const float phase    = glm::radians(180.0f) * static_cast<float>(i);
			const float orbAngle = t * glm::radians(25.0f) + phase;
			const float bob      = 0.6f * std::sin(t * 1.5f + phase);
			const glm::vec3 pos  = { 4.2f * std::cos(orbAngle), bob, 4.2f * std::sin(orbAngle) };

			glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, pos);
			m = glm::rotate(m, t * glm::radians(60.0f), diagAxis);
			m = glm::scale(m, { 0.7f, 0.7f, 0.7f });

			meow::Entity& e = (i == 0) ? m_orbitEntityA : m_orbitEntityB;
			context.world->Set(e, TransformComponent{ .localToWorld = m });
		}

		// ── Camera: slow circular orbit, looking at the origin ────────────────
		{
			const float camRad   = glm::radians(m_cameraAngle);
			const glm::vec3 eye  = { 7.0f * std::cos(camRad), 4.0f, 7.0f * std::sin(camRad) };

			const VkExtent2D extent = context.engine.GetSwapchainExtent();
			const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);

			glm::mat4 proj = glm::perspective(glm::radians(55.0f), aspect, 0.1f, 100.0f);
			proj[1][1] *= -1.0f;

			const glm::mat4 view = glm::lookAt(eye, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
			context.scene->SetViewProjection(proj * view);
		}
	}

	void SandboxLayer::OnGui(LayerContext& context)
	{
		(void)context;
	}
}
