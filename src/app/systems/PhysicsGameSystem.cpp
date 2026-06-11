#include "PhysicsGameSystem.hpp"

#include <cstdio>
#include <cmath>
#include <array>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include "camera/Camera.hpp"
#include "rendering/LightingManager.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/Components.hpp"
#include "scene/EcsHelpers.hpp"
#include "gpu/BindlessManager.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/Input.hpp"
#include "assets/AssetManager.hpp"
#include "utils/Logger.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether::app
{

	namespace
	{
		// Simple tag so OnUnregister can sweep all physics-demo entities in one pass.
		// Non-empty member avoids EnTT's empty-type optimization (emplace returns void for empty structs).
		struct PhysicsSceneTag
		{
			bool value = true;
		};

		struct ProjectileTag
		{
			bool value = true;
		};
	} // namespace

	// ── Init ──────────────────────────────────────────────────────────────────────

	void PhysicsGameSystem::Init(ServiceContainer& services, aether::AssetManager& assets, aether::CameraManager& cameras, aether::Input& input)
	{
		m_services = &services;
		m_assets = &assets;
		m_cameras = &cameras;
		m_input = &input;
	}

	// ── Scene construction ────────────────────────────────────────────────────────

	void PhysicsGameSystem::BuildScene(aether::World& world)
	{
		const aether::Mesh& cube = m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Cube);
		const aether::Mesh& sphere = m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Sphere);

		// ── Ground ────────────────────────────────────────────────────────────────
		{
			aether::EntityHandle ground = aether::ecs::SpawnMesh(world, m_pipeline, cube, m_groundMaterial, glm::translate(glm::mat4(1.f), {0.f, -kGroundThickness, 0.f}));
			ground.Add<PhysicsSceneTag>();
			ground.Add<aether::BoxBodyDesc>(aether::BoxBodyDesc{
			        .halfExtents = {kGroundHalfExtent, kGroundThickness, kGroundHalfExtent},
			        .motionType = PhysicsMotionType::Static,
			        .layer = aether::PhysicsLayer::NonMoving,
			});
			m_sceneEntities.push_back(ground);
		}

		// ── Boundary walls (4 sides, static) ─────────────────────────────────────
		const float wallH = 4.f;
		const float wallHalf = kGroundHalfExtent;
		const float wallT = 0.4f;

		struct WallDesc
		{
			glm::vec3 pos;
			glm::vec3 half;
		};

		const std::array<WallDesc, 4> walls = {{
		        {{0.f, wallH * 0.5f, wallHalf + wallT}, {wallHalf, wallH * 0.5f, wallT}},
		        {{0.f, wallH * 0.5f, -wallHalf - wallT}, {wallHalf, wallH * 0.5f, wallT}},
		        {{wallHalf + wallT, wallH * 0.5f, 0.f}, {wallT, wallH * 0.5f, wallHalf}},
		        {{-wallHalf - wallT, wallH * 0.5f, 0.f}, {wallT, wallH * 0.5f, wallHalf}},
		}};

		for (const WallDesc& wallDesc: walls)
		{
			aether::EntityHandle wall = aether::ecs::SpawnMesh(world, m_pipeline, cube, m_wallMaterial, glm::translate(glm::mat4(1.f), wallDesc.pos));
			wall.Add<PhysicsSceneTag>();
			wall.Add<aether::BoxBodyDesc>(aether::BoxBodyDesc{
			        .halfExtents = wallDesc.half,
			        .motionType = PhysicsMotionType::Static,
			        .layer = aether::PhysicsLayer::NonMoving,
			});
			m_sceneEntities.push_back(wall);
		}

		// ── Stacked boxes ─────────────────────────────────────────────────────────
		const float boxHalf = 0.5f;
		for (int row = 0; row < kStackHeight; ++row)
		{
			for (int col = 0; col < kStackWidth; ++col)
			{
				const float x = (col - (kStackWidth - 1) * 0.5f) * (boxHalf * 2.f + 0.02f);
				const float y = boxHalf + row * (boxHalf * 2.f + 0.01f) + kGroundThickness * 0.f;

				aether::EntityHandle box = aether::ecs::SpawnMesh(world, m_pipeline, cube, m_boxMaterial, glm::translate(glm::mat4(1.f), {x, y, 0.f}));
				box.Add<PhysicsSceneTag>();
				box.Add<aether::BoxBodyDesc>(aether::BoxBodyDesc{
				        .halfExtents = glm::vec3(boxHalf),
				        .motionType = PhysicsMotionType::Dynamic,
				        .restitution = 0.3f,
				});
				m_sceneEntities.push_back(box);
			}
		}

		// ── Scattered spheres ─────────────────────────────────────────────────────
		std::uniform_real_distribution<float> posDist(-kGroundHalfExtent * 0.65f, kGroundHalfExtent * 0.65f);
		std::uniform_real_distribution<float> heightDist(3.f, 14.f);
		std::uniform_real_distribution<float> radiusDist(0.25f, 0.55f);

		for (int scatterIdx = 0; scatterIdx < kScatterCount; ++scatterIdx)
		{
			const float radius = radiusDist(m_rng);
			const glm::vec3 pos = {posDist(m_rng), heightDist(m_rng), posDist(m_rng)};

			aether::EntityHandle scatterSphere = aether::ecs::SpawnMesh(world, m_pipeline, sphere, m_roundBodyMaterial, glm::translate(glm::mat4(1.f), pos));
			scatterSphere.Add<PhysicsSceneTag>();
			scatterSphere.Add<aether::SphereBodyDesc>(aether::SphereBodyDesc{
			        .radius = radius,
			        .motionType = PhysicsMotionType::Dynamic,
			        .restitution = 0.5f,
			});
			m_sceneEntities.push_back(scatterSphere);
		}
	}

	void PhysicsGameSystem::ClearScene(aether::World& world)
	{
		for (const aether::Entity e: m_sceneEntities)
		{
			if (world.Has<aether::RigidBodyComponent>(e))
			{
				m_physics->RemoveBody(world, e);
			}
			world.Destroy(e);
		}
		m_sceneEntities.clear();
		m_projectileCount = 0;
	}

	// ── OnRegister ────────────────────────────────────────────────────────────────

	void PhysicsGameSystem::OnRegister(aether::World& world)
	{
		if (m_services)
		{
			m_physics = &m_services->Get<aether::PhysicsSystem>();
		}

		AE_INFO(aether::LogCategory::App, "PhysicsGameSystem registered.");
		if (!m_services || !m_assets || !m_cameras || !m_input || !m_physics)
		{
			AE_WARN(aether::LogCategory::App, "PhysicsGameSystem not fully initialised - aborting.");
			return;
		}

		// ── Pipeline ──────────────────────────────────────────────────────────────
		const aether::gpu::DescriptorSetLayout bindlessLayout = m_services->Get<BindlessManager>().GetLayout();
		const aether::gpu::DescriptorSetLayout lightingLayout = m_services->Get<LightingManager>().GetSetLayout();
		const std::array<aether::gpu::DescriptorSetLayout, 2> setLayouts{bindlessLayout, lightingLayout};

		AE_EXPECT_OR_THROW(pipeline,
		        m_assets->CreateGraphicsPipeline({
		                .shaderVfsPath = "shaders://gltf_mesh.spv",
		                .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		                .depthFormat = gpu::ToVk(m_services->Get<Swapchain>().GetDepthFormat()),
		                .depthTestEnable = true,
		                .depthWriteEnable = true,
		                .setLayouts = std::span<const aether::gpu::DescriptorSetLayout>(setLayouts.data(), setLayouts.size()),
		        }));
		m_pipeline = std::move(pipeline);

		// ── Materials ─────────────────────────────────────────────────────────────
		m_groundMaterial = {};
		m_groundMaterial.baseColorFactor = glm::vec4(0.45f, 0.45f, 0.42f, 1.f);
		m_groundMaterial.roughnessFactor = 0.9f;
		m_groundMaterial.metallicFactor = 0.0f;
		m_assets->RegisterMaterial(m_groundMaterial);

		m_wallMaterial = {};
		m_wallMaterial.baseColorFactor = glm::vec4(0.35f, 0.38f, 0.42f, 1.f);
		m_wallMaterial.roughnessFactor = 0.85f;
		m_wallMaterial.metallicFactor = 0.0f;
		m_assets->RegisterMaterial(m_wallMaterial);

		m_boxMaterial = {};
		m_boxMaterial.baseColorFactor = glm::vec4(0.72f, 0.48f, 0.22f, 1.f);
		m_boxMaterial.roughnessFactor = 0.7f;
		m_boxMaterial.metallicFactor = 0.0f;
		m_assets->RegisterMaterial(m_boxMaterial);

		m_roundBodyMaterial = {};
		m_roundBodyMaterial.baseColorFactor = glm::vec4(0.28f, 0.55f, 0.78f, 1.f);
		m_roundBodyMaterial.roughnessFactor = 0.3f;
		m_roundBodyMaterial.metallicFactor = 0.6f;
		m_assets->RegisterMaterial(m_roundBodyMaterial);

		m_projectileMaterial = {};
		m_projectileMaterial.baseColorFactor = glm::vec4(0.85f, 0.22f, 0.15f, 1.f);
		m_projectileMaterial.roughnessFactor = 0.2f;
		m_projectileMaterial.metallicFactor = 0.8f;
		m_assets->RegisterMaterial(m_projectileMaterial);

		// ── Scene ─────────────────────────────────────────────────────────────────
		BuildScene(world);

		// ── Cameras ───────────────────────────────────────────────────────────────
		m_orbitCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Orbit,
		        .orbitTarget = {0.f, 4.f, 0.f},
		        .orbitDistance = 28.f,
		        .orbitYaw = 25.f,
		        .orbitPitch = 22.f,
		});

		m_freeCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Free,
		        .position = {0.f, 6.f, 22.f},
		        .yaw = 0.f,
		        .pitch = -12.f,
		        .moveSpeed = 10.f,
		        .lookSpeed = 0.14f,
		});

		m_cameras->SetMainCamera(m_orbitCamera);
	}

	// ── Projectile ────────────────────────────────────────────────────────────────

	void PhysicsGameSystem::FireProjectile(aether::World& world)
	{
		const aether::Mesh& sphere = m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Sphere);

		// Fire from slightly above and in front of the camera toward the target stack.
		const glm::vec3 spawnPos = {0.f, 3.f, kGroundHalfExtent - 1.f};
		const glm::vec3 direction = glm::normalize(glm::vec3{0.f, 0.2f, -1.f});

		aether::EntityHandle projectile = aether::ecs::SpawnMesh(world, m_pipeline, sphere, m_projectileMaterial, glm::translate(glm::mat4(1.f), spawnPos));
		projectile.Add<PhysicsSceneTag>();
		projectile.Add<ProjectileTag>();
		projectile.Add<aether::SphereBodyDesc>(aether::SphereBodyDesc{
		        .radius = kProjectileRadius,
		        .motionType = PhysicsMotionType::Dynamic,
		        .friction = 0.2f,
		        .restitution = 0.4f,
		        .initialVelocity = direction * kProjectileSpeed,
		});
		m_sceneEntities.push_back(projectile);

		++m_projectileCount;
	}

	// ── Update ────────────────────────────────────────────────────────────────────

	void PhysicsGameSystem::Update(aether::World& world, float dt)
	{
		if (!m_services || !m_cameras || !m_input || !m_physics)
		{
			return;
		}

		m_simTime += dt;
		m_projectileCooldown = std::max(0.f, m_projectileCooldown - dt);

		// Count active dynamic bodies for the HUD.
		m_activeBodyCount = 0;
		for (auto [entity, rigid]: world.View<aether::RigidBodyComponent>().each())
		{
			if (rigid.motionType == PhysicsMotionType::Dynamic)
			{
				++m_activeBodyCount;
			}
		}

		// ── Input ─────────────────────────────────────────────────────────────────

		// Space = fire a projectile (rate-limited)
		if (m_input->IsKeyPressed(aether::Key::Space) && m_projectileCooldown <= 0.f)
		{
			FireProjectile(world);
			m_projectileCooldown = kProjectileCooldownTime;
		}

		// R = rebuild the scene
		if (m_input->IsKeyPressed(aether::Key::R))
		{
			ClearScene(world);
			m_rng = std::mt19937{1337};
			m_simTime = 0.f;
			BuildScene(world);
			AE_INFO(aether::LogCategory::App, "Physics scene reset.");
		}

		// C = swap camera
		if (m_input->IsKeyPressed(aether::Key::C))
		{
			const aether::CameraHandle active = m_cameras->GetMainCamera();
			const aether::CameraHandle next = (active == m_orbitCamera) ? m_freeCamera : m_orbitCamera;
			m_cameras->SetMainCamera(next);
		}
	}

	// ── OnUnregister ──────────────────────────────────────────────────────────────

	void PhysicsGameSystem::OnUnregister(aether::World& world)
	{
		ClearScene(world);

		m_assets->UnregisterMaterial(m_groundMaterial);
		m_assets->UnregisterMaterial(m_wallMaterial);
		m_assets->UnregisterMaterial(m_boxMaterial);
		m_assets->UnregisterMaterial(m_roundBodyMaterial);
		m_assets->UnregisterMaterial(m_projectileMaterial);

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

		m_pipeline.Destroy();
		AE_INFO(aether::LogCategory::App, "PhysicsGameSystem unregistered.");
	}

} // namespace aether::app
