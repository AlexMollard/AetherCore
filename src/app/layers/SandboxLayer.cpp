#include "SandboxLayer.hpp"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "Components.hpp"
#include "FileSystem.hpp"
#include "Logger.hpp"
#include "Material.hpp"
#include "AetherCore.hpp"
#include "World.hpp"

namespace aether::app
{
	void SandboxLayer::OnAttach(LayerContext& context)
	{
		INFO(LogCategory::App, "Sandbox layer attached.");

		// Configure a visible default key light for PBR assets.
		context.engine.SetDirectionalLight(glm::vec3(0.35f, 0.88f, 0.31f), 4.5f);
		context.engine.SetAmbientLight(glm::vec3(0.12f, 0.13f, 0.15f));

		// ── Shared pipeline ───────────────────────────────────────────────────
		const VkDescriptorSetLayout bindlessLayout =
			context.engine.GetBindlessManager().GetLayout();

		m_pipeline = context.engine.CreateGraphicsPipeline({
			.shaderVfsPath = "shaders://gltf_mesh.slang.spv",
			.colorFormat = context.engine.GetForwardColorFormat(),
			.depthFormat = context.engine.GetSwapchainDepthFormat(),
			.depthTestEnable = true,
			.depthWriteEnable = true,
			.setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
			});

		m_cubeMesh = &context.engine.GetPrimitiveMesh(aether::PrimitiveMesh::Cube);
		m_quadMesh = &context.engine.GetPrimitiveMesh(aether::PrimitiveMesh::Quad);

		m_debugTexture = context.engine.CreateTexture("assets://textures/tex_DebugUVTiles.png");
		const uint32_t texSlot = m_debugTexture.GetBindlessSlot();

		m_debugTexturedMaterial = {};
		m_debugTexturedMaterial.albedoSlot = texSlot;
		context.engine.RegisterMaterial(m_debugTexturedMaterial);

		m_untexturedMaterial = {};
		context.engine.RegisterMaterial(m_untexturedMaterial);

		m_rttFeedMaterial = {};
		context.engine.RegisterMaterial(m_rttFeedMaterial);

		// Convenience lambdas to keep entity setup readable.
		auto pipe = [&]() { return PipelineComponent{ .pipeline = &m_pipeline }; };
		auto withTex = [&]() { return MaterialComponent{ .material = m_debugTexturedMaterial }; };
		auto noTex = [&]() { return MaterialComponent{ .material = m_untexturedMaterial }; };

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

		// ── Cameras ──────────────────────────────────────────────────────────
		m_orbitCamera = context.cameras->Create({
			.mode = CameraMode::Orbit,
			.orbitTarget = { 0.0f, 0.0f, 0.0f },
			.orbitDistance = 8.0f,
			.orbitYaw = 35.0f,
			.orbitPitch = 22.0f,
			});

		m_freeCamera = context.cameras->Create({
			.mode = CameraMode::Free,
			.position = { 0.0f, 2.5f, 9.0f },
			.yaw = 0.0f,
			.pitch = -12.0f,
			.moveSpeed = 6.0f,
			.lookSpeed = 0.14f,
			});

		m_rttCamera = context.cameras->Create({
			.mode = CameraMode::Orbit,
			.orbitTarget = { 0.0f, 0.0f, 0.0f },
			.orbitDistance = 11.0f,
			.orbitYaw = 0.0f,
			.orbitPitch = 62.0f,
			});

		context.cameras->SetMainCamera(m_orbitCamera);
		m_rttTarget = context.engine.CreateCameraRenderTarget(m_rttCamera, { 512, 512 });

		constexpr std::string_view kDemoGltfPath = "assets://models/Fox/Fox.gltf";
		if (io::FileSystem::Exists(kDemoGltfPath))
		{
			m_loadedGltf = context.engine.LoadGltfAsset(kDemoGltfPath);
			m_gltfEntities.reserve(m_loadedGltf->primitives.size());

			for (const LoadedGltfPrimitive& primitive : m_loadedGltf->primitives)
			{
				const Entity entity = context.world->CreateEntity();
				context.world->Set(entity, PipelineComponent{ .pipeline = &m_pipeline });
				context.world->Set(entity, MeshComponent{ .mesh = &primitive.mesh });
				context.world->Set(entity, MaterialComponent{ .material = primitive.material });
				
				// Scale fox down to 0.05x and position it at (0, 0, 0)
				glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f));
				transform = glm::scale(transform, glm::vec3(0.05f, 0.05f, 0.05f));
				transform = transform * primitive.localTransform;
				
				context.world->Set(entity, TransformComponent{ .localToWorld = transform });
				m_gltfEntities.push_back(entity);
			}

			INFO(LogCategory::App,
				"Loaded glTF scene from '{}' with {} render primitives.",
				kDemoGltfPath,
				m_loadedGltf->primitives.size());
		}
		else
		{
			INFO(LogCategory::App,
				"No demo glTF found at '{}'; skipping glTF scene load.",
				kDemoGltfPath);
		}

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
		for (const Entity e : m_gltfEntities)
			context.world->DestroyEntity(e);
		m_gltfEntities.clear();
		if (m_loadedGltf)
		{
			for (LoadedGltfPrimitive& primitive : m_loadedGltf->primitives)
			{
				context.engine.UnregisterMaterial(primitive.material);
			}
		}
		m_loadedGltf.reset();

		context.engine.UnregisterMaterial(m_rttFeedMaterial);
		context.engine.UnregisterMaterial(m_untexturedMaterial);
		context.engine.UnregisterMaterial(m_debugTexturedMaterial);

		if (m_rttTarget.IsValid())
		{
			context.engine.DestroyCameraRenderTarget(m_rttTarget);
			m_rttTarget = {};
		}
		if (m_orbitCamera.IsValid())
		{
			context.cameras->Destroy(m_orbitCamera);
			m_orbitCamera = {};
		}
		if (m_freeCamera.IsValid())
		{
			context.cameras->Destroy(m_freeCamera);
			m_freeCamera = {};
		}
		if (m_rttCamera.IsValid())
		{
			context.cameras->Destroy(m_rttCamera);
			m_rttCamera = {};
		}

		m_debugTexture.Destroy();
		m_cubeMesh = nullptr;
		m_quadMesh = nullptr;
		m_pipeline.Destroy();
	}

	void SandboxLayer::OnUpdate(LayerContext& context)
	{
		m_time += static_cast<float>(context.deltaTimeSeconds);
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
			const float step = glm::radians(360.0f / kRingCount);
			const float angle = t * glm::radians(40.0f) + static_cast<float>(i) * step;
			const glm::vec3 pos = { kRingRadius * std::cos(angle), 0.0f, kRingRadius * std::sin(angle) };
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
			const float phase = glm::radians(180.0f) * static_cast<float>(i);
			const float orbAngle = t * glm::radians(25.0f) + phase;
			const float bob = 0.6f * std::sin(t * 1.5f + phase);
			const glm::vec3 pos = { 4.2f * std::cos(orbAngle), bob, 4.2f * std::sin(orbAngle) };

			glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, pos);
			m = glm::rotate(m, t * glm::radians(60.0f), diagAxis);
			m = glm::scale(m, { 0.7f, 0.7f, 0.7f });

			aether::Entity& e = (i == 0) ? m_orbitEntityA : m_orbitEntityB;
			context.world->Set(e, TransformComponent{ .localToWorld = m });
		}

		// --- Fox rotation: slow spin to show off the loaded glTF asset's materials and textures ---
		{
			glm::mat4 m = glm::rotate(glm::mat4{ 1.0f }, t * glm::radians(15.0f), { 0.0f, 1.0f, 0.0f });
			m = glm::scale(m, { 0.05f, 0.05f, 0.05f });
			context.world->Set(m_gltfEntities.front(), TransformComponent{ .localToWorld = m });
		}

		// ── Keyboard: T = cycle tonemap, F = toggle FXAA ─────────────────────
		{
			const aether::Input& input = *context.input;

			if (input.IsKeyPressed(aether::Key::T))
			{
				const auto next = static_cast<aether::TonemapMode>(
					(static_cast<int>(context.engine.GetTonemapMode()) + 1) % 3);
				context.engine.SetTonemapMode(next);

				const char* names[] = { "Reinhard", "ACES Filmic", "Uncharted2" };
				INFO(LogCategory::App, "Tonemap: {}", names[static_cast<int>(next)]);
			}

			if (input.IsKeyPressed(aether::Key::F))
			{
				const bool enabled = !context.engine.IsFxaaEnabled();
				context.engine.SetFxaaEnabled(enabled);
				INFO(LogCategory::App, "FXAA: {}", enabled ? "on" : "off");
			}

			// C switches between orbit and free cameras at runtime.
			if (input.IsKeyPressed(aether::Key::C))
			{
				const CameraHandle active = context.cameras->GetMainCamera();
				const CameraHandle next = (active == m_orbitCamera) ? m_freeCamera : m_orbitCamera;
				context.cameras->SetMainCamera(next);
				INFO(LogCategory::App, "Main camera: {}", (next == m_freeCamera) ? "Free" : "Orbit");
			}
		}

		// Spin the RTT camera around the scene so the RTT texture is visibly live.
		if (Camera* cam = context.cameras->TryGet(m_rttCamera))
		{
			cam->SetOrbitYawPitch(t * 18.0f, 62.0f);
		}

		// Feed RTT output into one cube once the texture has a valid bindless slot.
		const uint32_t rtSlot = context.engine.GetRenderTargetBindlessSlot(m_rttTarget);
		if (rtSlot != Material::kNoTexture)
		{
			m_rttFeedMaterial.albedoSlot = rtSlot;
			context.engine.RegisterMaterial(m_rttFeedMaterial);
			context.world->Set(m_orbitEntityB, MaterialComponent{
				.material = m_rttFeedMaterial
				});
		}
	}

	void SandboxLayer::OnGui(LayerContext& context)
	{
		(void)context;
	}
}
