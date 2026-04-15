#include "SandboxGameSystem.hpp"

#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

#include "World.hpp"
#include "EcsHelpers.hpp"
#include "Logger.hpp"
#include "AetherCore.hpp"
#include "AssetManager.hpp"
#include "Input.hpp"
#include "Camera.hpp"
#include "FileSystem.hpp"

namespace aether::app
{
	namespace
	{
		struct SandboxEntityTag { bool value = true; };
		struct GroundTag { bool value = true; };
		struct CenterTag { bool value = true; };
		struct RingTag { int  index = 0; };
		struct OrbitTag { float phase = 0.0f; bool isRttTarget = false; };
		struct SandboxModelTag { bool value = true; };
	}

	std::uint32_t SandboxGameSystem::GetAnimationCount() const
	{
		if (!m_model || !m_model->animator)
		{
			return 0;
		}

		return m_model->animator->GetAnimationCount();
	}

	std::string_view SandboxGameSystem::GetCurrentAnimationName() const
	{
		if (!m_model || !m_model->animator)
		{
			return {};
		}

		const std::uint32_t animationIndex = m_model->animator->GetCurrentAnimation();
		return m_model->animator->GetAnimationName(animationIndex);
	}

	void SandboxGameSystem::Init(aether::AetherCore& engine,
		aether::AssetManager& assets,
		aether::CameraManager& cameras,
		aether::Input& input)
	{
		m_engine = &engine;
		m_assets = &assets;
		m_cameras = &cameras;
		m_input = &input;
	}

	void SandboxGameSystem::OnRegister(aether::World& world)
	{
		INFO(aether::LogCategory::App, "SandboxGameSystem registered.");
		if (!m_engine || !m_assets || !m_cameras || !m_input)
		{
			WARN(aether::LogCategory::App, "SandboxGameSystem not initialized with dependencies!");
			return;
		}

		// ── Shared pipeline ───────────────────────────────────────────────────
		const VkDescriptorSetLayout bindlessLayout =
			m_engine->GetBindlessManager().GetLayout();
		const VkDescriptorSetLayout lightingLayout =
			m_engine->GetLightingSetLayout();
		const std::array<VkDescriptorSetLayout, 2> setLayouts{ bindlessLayout, lightingLayout };

		m_pipeline = m_assets->CreateGraphicsPipeline({
			.shaderVfsPath = "shaders://gltf_mesh.slang.spv",
			.colorFormat = aether::AetherCore::GetForwardColorFormat(),
			.depthFormat = m_engine->GetSwapchainDepthFormat(),
			.depthTestEnable = true,
			.depthWriteEnable = true,
			.setLayouts = std::span<const VkDescriptorSetLayout>(setLayouts.data(), setLayouts.size()),
			});

		m_cubeMesh = &m_engine->GetPrimitiveMesh(aether::PrimitiveMesh::Cube);
		m_quadMesh = &m_engine->GetPrimitiveMesh(aether::PrimitiveMesh::Quad);

		m_debugTexture = m_assets->CreateTexture("assets://textures/tex_DebugUVTiles.png");
		const uint32_t texSlot = m_debugTexture.GetBindlessSlot();

		m_debugTexturedMaterial = {};
		m_debugTexturedMaterial.albedoSlot = texSlot;
		m_assets->RegisterMaterial(m_debugTexturedMaterial);

		m_untexturedMaterial = {};
		m_assets->RegisterMaterial(m_untexturedMaterial);

		m_rttFeedMaterial = {};
		m_assets->RegisterMaterial(m_rttFeedMaterial);

		// ── Ground quad (textured) ────────────────────────────────────────────
		{
			const aether::Entity e = aether::ecs::SpawnMesh(world, m_pipeline, *m_quadMesh, m_debugTexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(e, SandboxEntityTag{});
			world.EmplaceOrReplace<GroundTag>(e, GroundTag{});
		}

		// ── Centre cube (textured, multi-axis spin) ───────────────────────────
		{
			const aether::Entity e = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_debugTexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(e, SandboxEntityTag{});
			world.EmplaceOrReplace<CenterTag>(e, CenterTag{});
		}

		// ── Ring of 8 small cubes (vertex colour, no texture) ─────────────────
		for (int i = 0; i < kRingCount; ++i)
		{
			const aether::Entity e = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_untexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(e, SandboxEntityTag{});
			world.EmplaceOrReplace<RingTag>(e, RingTag{ .index = i });
		}

		// ── Two wider-orbit cubes (one textured, one vertex colour) ───────────
		{
			const aether::Entity eA = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_debugTexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(eA, SandboxEntityTag{});
			world.EmplaceOrReplace<OrbitTag>(eA, OrbitTag{ .phase = 0.0f, .isRttTarget = false });

			const aether::Entity eB = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_untexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(eB, SandboxEntityTag{});
			world.EmplaceOrReplace<OrbitTag>(eB, OrbitTag{ .phase = glm::radians(180.0f), .isRttTarget = true });
		}

		// ── Cameras ──────────────────────────────────────────────────────────
		m_orbitCamera = m_cameras->Create({
			.mode = aether::CameraMode::Orbit,
			.orbitTarget = { 0.0f, 0.0f, 0.0f },
			.orbitDistance = 8.0f,
			.orbitYaw = 35.0f,
			.orbitPitch = 22.0f,
			});

		m_freeCamera = m_cameras->Create({
			.mode = aether::CameraMode::Free,
			.position = { 0.0f, 2.5f, 9.0f },
			.yaw = 0.0f,
			.pitch = -12.0f,
			.moveSpeed = 6.0f,
			.lookSpeed = 0.14f,
			});

		m_rttCamera = m_cameras->Create({
			.mode = aether::CameraMode::Orbit,
			.orbitTarget = { 0.0f, 0.0f, 0.0f },
			.orbitDistance = 11.0f,
			.orbitYaw = 0.0f,
			.orbitPitch = 62.0f,
			});

		m_cameras->SetMainCamera(m_orbitCamera);
		m_rttTarget = m_engine->CreateCameraRenderTarget(m_rttCamera, { 512, 512 });

		// ── Light demo (small grid so tile culling is easy to reason about) ──
		{
			std::vector<aether::Renderer::PointLight> pointLights;
			pointLights.reserve(9);
			for (int z = -1; z <= 1; ++z)
			{
				for (int x = -1; x <= 1; ++x)
				{
					aether::Renderer::PointLight l{};
					l.position = glm::vec3(
						static_cast<float>(x) * 2.0f,
						0.5f,
						static_cast<float>(z) * 2.0f);
					l.radius = 3.0f;
					l.intensity = 1.5f;
					l.color = glm::vec3(
						0.55f + 0.45f * std::sin(static_cast<float>(x) * 0.21f),
						0.55f + 0.45f * std::sin(static_cast<float>(z) * 0.23f + 1.7f),
						0.55f + 0.45f * std::sin(static_cast<float>(x - z) * 0.17f + 3.1f));
					pointLights.push_back(l);
				}
			}
			m_engine->GetRenderer().SetPointLights(std::move(pointLights));
		}

		// ── Load demo glTF ──────────────────────────────────────────────────────
		constexpr std::string_view kDemoGltfPath = "assets://models/Fox/Fox.gltf";
		if (aether::io::FileSystem::Exists(kDemoGltfPath))
		{
			m_model = m_assets->LoadModel(kDemoGltfPath);
			m_modelEntityCount = aether::ecs::SpawnModel(
				world, *m_assets, *m_model, m_pipeline, 0.05f,
				SandboxEntityTag{}, SandboxModelTag{});

			if (m_model->animator)
			{
				const std::uint32_t animCount = m_model->animator->GetAnimationCount();
				INFO(aether::LogCategory::App, "glTF has {} animation(s):", animCount);
				for (std::uint32_t i = 0; i < animCount; ++i)
					INFO(aether::LogCategory::App, "  [{}] {}", i, m_model->animator->GetAnimationName(i));
				m_model->animator->SetAnimation(2);
			}

			INFO(aether::LogCategory::App,
				"Loaded glTF scene from '{}' with {} render primitives.",
				kDemoGltfPath,
				m_model->primitives.size());
		}
		else
		{
			INFO(aether::LogCategory::App,
				"No demo glTF found at '{}'; skipping glTF scene load.",
				kDemoGltfPath);
		}

		INFO(aether::LogCategory::App, "Scene built: ground + centre + {} ring + 2 orbit cubes.", kRingCount);
	}

	void SandboxGameSystem::Update(aether::World& world, float dt)
	{
		if (!m_engine || !m_cameras || !m_input)
			return;

		m_time += dt;

		// ── Ground: flat quad, 10×10, lying on Y = -0.5 ──────────────────────
		{
			glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, { 0.0f, -0.5f, 0.0f });
			m = glm::rotate(m, glm::radians(-90.0f), { 1.0f, 0.0f, 0.0f });
			m = glm::scale(m, { 10.0f, 10.0f, 1.0f });
			auto gView = world.View<GroundTag, aether::TransformComponent>();
			for (auto e : gView)
				gView.get<aether::TransformComponent>(e).localToWorld = m;
		}

		// ── Centre cube: slow dual-axis spin, slightly scaled up ──────────────
		{
			glm::mat4 m = glm::rotate(glm::mat4{ 1.0f }, m_time * glm::radians(20.0f), { 0.0f, 1.0f, 0.0f });
			m = glm::rotate(m, m_time * glm::radians(9.0f), { 1.0f, 0.0f, 0.0f });
			m = glm::scale(m, { 1.2f, 1.2f, 1.2f });
			auto cView = world.View<CenterTag, aether::TransformComponent>();
			for (auto e : cView)
				cView.get<aether::TransformComponent>(e).localToWorld = m;
		}

		// ── Ring: 8 cubes revolving around the origin ─────────────────────────
		{
			constexpr float kRingRadius = 2.8f;
			const float kStep = glm::radians(360.0f / static_cast<float>(kRingCount));
			auto rView = world.View<RingTag, aether::TransformComponent>();
			for (auto e : rView)
			{
				const int i = rView.get<RingTag>(e).index;
				const float angle = m_time * glm::radians(40.0f) + static_cast<float>(i) * kStep;
				const glm::vec3 pos = { kRingRadius * std::cos(angle), 0.0f, kRingRadius * std::sin(angle) };
				const float selfSpin = m_time * glm::radians(90.0f + static_cast<float>(i) * 15.0f);

				glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, pos);
				m = glm::rotate(m, selfSpin, { 0.0f, 1.0f, 0.0f });
				m = glm::scale(m, { 0.35f, 0.35f, 0.35f });
				rView.get<aether::TransformComponent>(e).localToWorld = m;
			}
		}

		// ── Orbit pair: 180° apart, wider radius, gentle Y bob ────────────────
		{
			const glm::vec3 diagAxis = glm::normalize(glm::vec3{ 1.0f, 1.0f, 0.3f });
			auto oView = world.View<OrbitTag, aether::TransformComponent>();
			for (auto e : oView)
			{
				const float phase = oView.get<OrbitTag>(e).phase;
				const float orbAngle = m_time * glm::radians(25.0f) + phase;
				const float bob = 0.6f * std::sin(m_time * 1.5f + phase);
				const glm::vec3 pos = { 4.2f * std::cos(orbAngle), bob, 4.2f * std::sin(orbAngle) };

				glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, pos);
				m = glm::rotate(m, m_time * glm::radians(60.0f), diagAxis);
				m = glm::scale(m, { 0.7f, 0.7f, 0.7f });
				oView.get<aether::TransformComponent>(e).localToWorld = m;
			}
		}

		// ── Model rotation: slow spin to show off the loaded asset ────────────
		{
			glm::mat4 m = glm::rotate(glm::mat4{ 1.0f }, m_time * glm::radians(15.0f), { 0.0f, 1.0f, 0.0f });
			m = glm::scale(m, { 0.05f, 0.05f, 0.05f });
			auto modelView = world.View<SandboxModelTag, aether::SkinComponent, aether::TransformComponent>();
			for (auto e : modelView)
				modelView.get<aether::TransformComponent>(e).localToWorld = m;
		}

		// ── Keyboard: T = cycle tonemap, F = toggle FXAA ─────────────────────
		{
			if (m_input->IsKeyPressed(aether::Key::T))
			{
				const auto next = static_cast<aether::TonemapMode>(
					(static_cast<int>(m_engine->GetTonemapMode()) + 1) % 3);
				m_engine->GetRenderer().SetTonemapMode(next);

				const char* names[] = { "Reinhard", "ACES Filmic", "Uncharted2" };
				INFO(aether::LogCategory::App, "Tonemap: {}", names[static_cast<int>(next)]);
			}

			if (m_input->IsKeyPressed(aether::Key::F))
			{
				const bool enabled = !m_engine->GetRenderer().IsFxaaEnabled();
				m_engine->GetRenderer().SetFxaaEnabled(enabled);
				INFO(aether::LogCategory::App, "FXAA: {}", enabled ? "on" : "off");
			}

			// C switches between orbit and free cameras at runtime.
			if (m_input->IsKeyPressed(aether::Key::C))
			{
				const aether::CameraHandle active = m_cameras->GetMainCamera();
				const aether::CameraHandle next = (active == m_orbitCamera) ? m_freeCamera : m_orbitCamera;
				m_cameras->SetMainCamera(next);
				INFO(aether::LogCategory::App, "Main camera: {}", (next == m_freeCamera) ? "Free" : "Orbit");
			}
		}

		// ── Spin the RTT camera around the scene ──────────────────────────────
		if (aether::Camera* cam = m_cameras->TryGet(m_rttCamera))
		{
			cam->SetOrbitYawPitch(m_time * 18.0f, 62.0f);
		}

		// ── Feed RTT output into one cube ────────────────────────────────────
		const uint32_t rtSlot = m_engine->GetRenderTargetBindlessSlot(m_rttTarget);
		if (rtSlot != aether::Material::kNoTexture)
		{
			m_rttFeedMaterial.albedoSlot = rtSlot;
			m_assets->RegisterMaterial(m_rttFeedMaterial);
			auto rttView = world.View<OrbitTag, aether::MaterialComponent>();
			for (auto e : rttView)
			{
				if (rttView.get<OrbitTag>(e).isRttTarget)
				{
					rttView.get<aether::MaterialComponent>(e).material = m_rttFeedMaterial;
					break;
				}
			}
		}
	}

	void SandboxGameSystem::OnUnregister(aether::World& world)
	{
		if (!m_engine || !m_assets || !m_cameras)
			return;

		INFO(aether::LogCategory::App, "SandboxGameSystem unregistered.");

		// Clean up all sandbox entities in one pass.
		std::vector<entt::entity> toDestroy;
		{
			auto allView = world.View<SandboxEntityTag>();
			toDestroy.assign(allView.begin(), allView.end());
		}
		for (const entt::entity e : toDestroy)
			world.Destroy(aether::Entity{ static_cast<std::uint32_t>(entt::to_integral(e)) });
		m_modelEntityCount = 0;

		// Unregister materials
		if (m_model)
		{
			for (aether::LoadedModelPrimitive& primitive : m_model->primitives)
			{
				m_assets->UnregisterMaterial(primitive.material);
			}
		}
		m_model.reset();

		m_assets->UnregisterMaterial(m_rttFeedMaterial);
		m_assets->UnregisterMaterial(m_untexturedMaterial);
		m_assets->UnregisterMaterial(m_debugTexturedMaterial);

		// Clean up cameras
		m_engine->DestroyCameraRenderTarget(m_rttTarget);
		m_rttTarget = {};

		if (m_orbitCamera.IsValid())
		{
			m_cameras->Destroy(m_orbitCamera);
			m_orbitCamera = {};
		}
		if (m_freeCamera.IsValid())
		{
			m_cameras->Destroy(m_freeCamera);
			m_freeCamera = {};
		}
		if (m_rttCamera.IsValid())
		{
			m_cameras->Destroy(m_rttCamera);
			m_rttCamera = {};
		}

		m_engine->GetRenderer().ClearPointLights();
		m_engine->GetRenderer().ClearSpotLights();

		// Clean up resources
		m_debugTexture.Destroy();
		m_cubeMesh = nullptr;
		m_quadMesh = nullptr;
		m_pipeline.Destroy();
	}
}
