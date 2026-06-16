#include "SandboxGameSystem.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <thread>

#include "camera/Camera.hpp"
#include "effects/EffectManager.hpp"
#include "rendering/LightingManager.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/RenderTargetService.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "scene/EcsHelpers.hpp"
#include "io/FileSystem.hpp"
#include "platform/Input.hpp"
#include "assets/AssetManager.hpp"
#include "utils/ServiceContainer.hpp"
#include "utils/Logger.hpp"
#include "scene/World.hpp"

namespace aether::app
{
	// Debug: artificial delay per loading step so the loading screen is visible.
	// Remove or set to 0ms for production.
	inline constexpr auto kDebugLoadDelay = std::chrono::milliseconds(100);

	namespace
	{
		struct RingComponent
		{
			int index = 0;
		};

		struct OrbitComponent
		{
			float phase = 0.0f;
			bool isRttTarget = false;
		};

		struct PointLightMarkerComponent
		{
			int index = 0;
		};

		struct FoxInstanceIndex
		{
			int index = 0;
		};
	} // namespace

	std::uint32_t SandboxGameSystem::GetAnimationCount() const
	{
		return (m_foxModel && m_foxModel->animationDb.IsValid()) ? m_foxModel->animationDb.GetClipCount() : 0u;
	}

	std::string_view SandboxGameSystem::GetCurrentAnimationName() const
	{
		if (!m_foxModel || !m_foxModel->animationDb.IsValid())
		{
			return {};
		}
		if (!m_world || m_foxInstances.empty() || m_foxInstances[0].empty())
		{
			return {};
		}
		const auto* smc = m_world->TryGet<aether::SkinnedMeshComponent>(m_foxInstances[0][0]);
		if (!smc)
		{
			return {};
		}
		return m_foxModel->animationDb.GetClipName(smc->clipIndex);
	}

	void SandboxGameSystem::Init(ServiceContainer& services, aether::AssetManager& assets, aether::CameraManager& cameras, aether::Input& input)
	{
		m_services = &services;
		m_assets = &assets;
		m_cameras = &cameras;
		m_input = &input;
	}

	void SandboxGameSystem::OnRegister(aether::World& world)
	{
		AE_INFO(aether::LogCategory::App, "SandboxGameSystem registered.");
		if (!m_services || !m_assets || !m_cameras || !m_input)
		{
			AE_WARN(aether::LogCategory::App, "SandboxGameSystem not initialized with dependencies!");
			return;
		}

		m_world = &world;

		// -- Shared pipeline (fast, do synchronously) --------------------------
		const aether::gpu::DescriptorSetLayout bindlessLayout = m_services->Get<BindlessManager>().GetLayout();
		const aether::gpu::DescriptorSetLayout lightingLayout = m_services->Get<LightingManager>().GetSetLayout();
		const std::array<aether::gpu::DescriptorSetLayout, 2> setLayouts{bindlessLayout, lightingLayout};

		AE_EXPECT_OR_THROW(pipeline,
		        m_assets->CreateGraphicsPipeline({
		                .shaderVfsPath = "shaders://gltf_mesh.spv",
		                .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		                .depthFormat = m_services->Get<Swapchain>().GetDepthFormat(),
		                .depthTestEnable = true,
		                .depthWriteEnable = true,
		                .setLayouts = std::span<const aether::gpu::DescriptorSetLayout>(setLayouts.data(), setLayouts.size()),
		        }));
		m_pipeline = std::move(pipeline);

		// -- Plasma effect pipeline ---------------------------------------------
		{
			aether::Material plasmaMat{};
			plasmaMat.baseColorFactor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
			plasmaMat.emissiveFactor = glm::vec3(1.0f, 0.3f, 0.8f);
			plasmaMat.metallicFactor = 0.5f;
			plasmaMat.roughnessFactor = 2.0f;
			plasmaMat.occlusionStrength = 0.8f;

			const bool ok = m_effectManager.CreateAndRegister("plasma", *m_assets, bindlessLayout, lightingLayout, aether::PostProcessStack::GetForwardColorFormat(), m_services->Get<Swapchain>().GetDepthFormat(), "shaders://plasma.spv", plasmaMat);
			if (!ok)
			{
				AE_WARN(aether::LogCategory::App, "SandboxGameSystem: failed to create plasma pipeline");
			}
		}

		m_cubeMesh = &m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Cube);
		m_quadMesh = &m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Quad);
		m_planeMesh = &m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Plane);

		// -- Deferred loading tasks --------------------------------------------
		// Obtain the shared LoadingManager registered by Application.
		m_loadingManager = &m_services->Get<LoadingManager>();

		m_loadingManager->AddTask([this] { LoadMaterials(); }, "Loading materials");

		m_loadingManager->AddTask([this] { LoadFoxModel(); }, "Loading models");

		m_loadingManager->AddTask([this] { SpawnSceneEntities(); }, "Spawning scene");

		m_loadingManager->AddTask([this] { CreateCameras(); }, "Creating cameras");

		m_loadingManager->AddTask([this] { CreatePointLights(); }, "Creating lights");
	}

	// -- Deferred loading helpers ---------------------------------------------
	// These are invoked one-per-frame by LoadingManager from Update().

	void SandboxGameSystem::LoadMaterials()
	{
		std::this_thread::sleep_for(kDebugLoadDelay);
		constexpr std::string_view kDebugMaterialPreset = "assets://materials/MyPBRFolder";
		if (aether::io::FileSystem::Exists(kDebugMaterialPreset))
		{
			AE_EXPECT_OR_THROW(mat, m_assets->LoadMaterialPreset(kDebugMaterialPreset, m_debugMaterialTextures));
			m_debugTexturedMaterial = mat;
		}
		else
		{
			m_debugMaterialTextures.clear();
			AE_EXPECT_OR_THROW(tex, m_assets->CreateTexture("assets://textures/tex_DebugUVTiles.png"));
			m_debugMaterialTextures.push_back(std::move(tex));
			m_debugTexturedMaterial = {};
			m_debugTexturedMaterial.albedoSlot = m_debugMaterialTextures.back().GetBindlessSlot();
			m_assets->RegisterMaterial(m_debugTexturedMaterial);
		}

		m_untexturedMaterial = {};
		m_untexturedMaterial.baseColorFactor = glm::vec4(0.56f, 0.60f, 0.64f, 1.0f);
		m_untexturedMaterial.roughnessFactor = 0.95f;
		m_untexturedMaterial.metallicFactor = 0.0f;
		m_assets->RegisterMaterial(m_untexturedMaterial);

		m_rttFeedMaterial = {};
		m_assets->RegisterMaterial(m_rttFeedMaterial);
	}

	void SandboxGameSystem::LoadFoxModel()
	{
		std::this_thread::sleep_for(kDebugLoadDelay);
		constexpr std::string_view kFoxPath = "assets://models/Fox/Fox.mesh";
		if (aether::io::FileSystem::Exists(kFoxPath))
		{
			try
			{
				AE_EXPECT_OR_THROW(model, m_assets->LoadModel(kFoxPath));
				m_foxModel = std::move(model);
				AE_INFO(aether::LogCategory::App,
				        "Fox model: {} primitives, animDb valid={}, skins={}, nodes={}",
				        m_foxModel->primitives.size(),
				        m_foxModel->animationDb.IsValid(),
				        m_foxModel->animationDb.GetSkinCount(),
				        m_foxModel->animationDb.GetNodeCount());
				if (m_foxModel->animationDb.IsValid())
				{
					const std::uint32_t animCount = m_foxModel->animationDb.GetClipCount();
					AE_INFO(aether::LogCategory::App, "Fox glTF has {} animation(s):", animCount);
					for (std::uint32_t i = 0; i < animCount; ++i)
					{
						AE_INFO(aether::LogCategory::App, "  [{}] '{}' (duration={:.2f}s)", i, m_foxModel->animationDb.GetClipName(i), m_foxModel->animationDb.GetClipDuration(i));
					}
				}

				// Log first few primitives' skin info
				for (std::size_t pi = 0; pi < m_foxModel->primitives.size() && pi < 2; ++pi)
				{
					const auto& prim = m_foxModel->primitives[pi];
					AE_INFO(aether::LogCategory::App, "  Prim[{}]: skinIndex={}, mesh={}", pi, prim.skinIndex, prim.mesh.IsValid());
				}
			}
			catch (const std::exception& e)
			{
				AE_WARN(aether::LogCategory::App, "Fox model load failed (re-run AssetPacker to regenerate): {}", e.what());
			}
		}
		else
		{
			AE_INFO(aether::LogCategory::App, "Fox model not found at '{}'; skipping.", kFoxPath);
		}
	}

	void SandboxGameSystem::SpawnSceneEntities()
	{
		std::this_thread::sleep_for(kDebugLoadDelay);
		World* world = m_world;
		if (!world)
		{
			return;
		}

		// -- Ground: large tiling plane ----------------------------------------
		{
			const aether::Entity e = aether::ecs::SpawnMesh(*world, m_pipeline, *m_planeMesh, m_debugTexturedMaterial);
			m_groundEntity = e;
			m_sandboxEntities.push_back(e);
		}

		// -- Foxes -------------------------------------------------------------
		if (m_foxModel.has_value())
		{
			std::uniform_real_distribution<float> posDist(-kGroundHalfExtent, kGroundHalfExtent);
			std::uniform_real_distribution<float> angleDist(0.0f, glm::two_pi<float>());
			std::uniform_real_distribution<float> timerDist(0.2f, 2.0f);

			m_foxAgents.reserve(kFoxCount);
			m_foxInstances.reserve(kFoxCount);

			for (int i = 0; i < kFoxCount; ++i)
			{
				FoxAgent agent;
				agent.pos = {posDist(m_rng), 0.0f, posDist(m_rng)};
				agent.heading = angleDist(m_rng);
				agent.target = {posDist(m_rng), 0.0f, posDist(m_rng)};
				agent.stateTimer = timerDist(m_rng);
				agent.idle = false;
				m_foxAgents.push_back(agent);

				std::vector<aether::Entity> instances = m_assets->SpawnModel(*m_foxModel, m_pipeline, 0, 0.05f);

				// Set initial animation state on each spawned SkinnedMeshComponent.
				if (m_foxModel->animationDb.IsValid())
				{
					const float runDur = m_foxModel->animationDb.GetClipDuration(kAnimRun);
					std::uniform_real_distribution<float> phaseDist(0.0f, runDur > 0.f ? runDur : 1.0f);
					const float phase = runDur > 0.f ? phaseDist(m_rng) : 0.f;
					for (const aether::Entity e: instances)
					{
						if (auto* smc = world->TryGet<aether::SkinnedMeshComponent>(e))
						{
							smc->clipIndex = kAnimRun;
							smc->playbackSpeed = kFoxAnimRunSpeed;
							smc->animTime = phase;
						}
					}
				}

				m_foxInstances.push_back(std::move(instances));
			}
			AE_INFO(aether::LogCategory::App, "Spawned {} fox instances.", kFoxCount);
		}

		// -- Sky cubes ---------------------------------------------------------
		// Centre spinning cube
		{
			const aether::Entity e = aether::ecs::SpawnMesh(*world, m_pipeline, *m_cubeMesh, m_debugTexturedMaterial);
			m_centerEntity = e;
			m_sandboxEntities.push_back(e);
		}

		// Ring of 8 small cubes
		for (int i = 0; i < kRingCount; ++i)
		{
			const aether::Entity e = aether::ecs::SpawnMesh(*world, m_pipeline, *m_cubeMesh, m_untexturedMaterial);
			world->EmplaceOrReplace<RingComponent>(e, RingComponent{.index = i});
			m_sandboxEntities.push_back(e);
		}

		// Wide-orbit pair - one textured, one RTT-fed
		{
			const aether::Entity eA = aether::ecs::SpawnMesh(*world, m_pipeline, *m_cubeMesh, m_debugTexturedMaterial);
			world->EmplaceOrReplace<OrbitComponent>(eA, OrbitComponent{.phase = 0.0f, .isRttTarget = false});
			m_sandboxEntities.push_back(eA);

			const aether::Entity eB = aether::ecs::SpawnMesh(*world, m_pipeline, *m_cubeMesh, m_untexturedMaterial);
			world->EmplaceOrReplace<OrbitComponent>(eB, OrbitComponent{.phase = glm::radians(180.0f), .isRttTarget = true});
			m_sandboxEntities.push_back(eB);
		}
	}

	void SandboxGameSystem::CreateCameras()
	{
		std::this_thread::sleep_for(kDebugLoadDelay);
		// Main orbit: pulled back far enough to see the entire fox field.
		m_orbitCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Orbit,
		        .orbitTarget = {0.0f, 0.0f, 0.0f},
		        .orbitDistance = 48.0f,
		        .orbitYaw = 35.0f,
		        .orbitPitch = 28.0f,
		});

		m_freeCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Free,
		        .position = {0.0f, 5.0f, 46.0f},
		        .yaw = 0.0f,
		        .pitch = -8.0f,
		        .moveSpeed = 12.0f,
		        .lookSpeed = 0.14f,
		});

		// RTT camera: top-down-ish view orbiting above the fox field.
		m_rttCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Orbit,
		        .orbitTarget = {0.0f, 0.0f, 0.0f},
		        .orbitDistance = 40.0f,
		        .orbitYaw = 0.0f,
		        .orbitPitch = 68.0f,
		});

		m_cameras->SetMainCamera(m_orbitCamera);
		AE_EXPECT_OR_THROW(targetId, m_services->Get<RenderTargetService>().CreateCameraRenderTarget(m_rttCamera.id, aether::gpu::Extent2D{512, 512}));
		m_rttTargetId = targetId;
	}

	void SandboxGameSystem::CreatePointLights()
	{
		std::this_thread::sleep_for(kDebugLoadDelay);
		World* world = m_world;
		if (!world)
		{
			return;
		}

		std::mt19937 lightRng(7);
		std::uniform_real_distribution<float> lightPosDist(-80.0f, 80.0f);
		const int lightCount = 128;
		m_pointLights.clear();
		m_pointLightMarkerMaterials.clear();
		m_pointLights.reserve(lightCount);
		m_pointLightMarkerMaterials.reserve(lightCount);
		for (int li = 0; li < lightCount; ++li)
		{
			const float t = static_cast<float>(li) / static_cast<float>(lightCount);
			const glm::vec3 lightColor = glm::vec3(0.55f + 0.45f * std::cos(glm::two_pi<float>() * (t + 0.00f)), 0.55f + 0.45f * std::cos(glm::two_pi<float>() * (t + 0.33f)), 0.55f + 0.45f * std::cos(glm::two_pi<float>() * (t + 0.66f)));

			aether::Renderer::PointLight l{};
			l.position = {lightPosDist(lightRng), 0.35f, lightPosDist(lightRng)};
			l.radius = 14.0f;
			l.intensity = 1.8f;
			l.color = lightColor;
			m_pointLights.push_back(l);

			aether::Material marker{};
			marker.baseColorFactor = glm::vec4(l.color * 0.20f, 1.0f);
			marker.emissiveFactor = l.color * 3.0f;
			marker.roughnessFactor = 0.9f;
			marker.metallicFactor = 0.0f;
			m_assets->RegisterMaterial(marker);
			m_pointLightMarkerMaterials.push_back(marker);

			const aether::Entity markerEntity = aether::ecs::SpawnMesh(*world, m_pipeline, *m_cubeMesh, m_pointLightMarkerMaterials.back());
			world->EmplaceOrReplace<PointLightMarkerComponent>(markerEntity, PointLightMarkerComponent{.index = li});
			m_sandboxEntities.push_back(markerEntity);
		}
		m_services->Get<Renderer>().SetPointLights(m_pointLights);
		const auto rendererLights = m_services->Get<Renderer>().GetPointLights();
		m_pointLights.assign(rendererLights.begin(), rendererLights.end());

		AE_INFO(aether::LogCategory::App, "Scene built: {} foxes on ground + {} ring + 1 center + 2 orbit sky cubes.", kFoxCount, kRingCount);
	}

	void SandboxGameSystem::Update(aether::World& world, float dt)
	{
		if (!m_services || !m_cameras || !m_input)
		{
			return;
		}

		// While loading tasks remain, process one per frame so the render
		// thread can display a loading screen between task steps.
		if (m_loadingManager && !m_loadingManager->IsComplete())
		{
			m_loadingManager->Update();
			return;
		}

		m_time += dt;

		// -- Ground: large flat quad, 56x56 units, lying at Y=0 ---------------
		{
			glm::mat4 m = glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), {1.0f, 0.0f, 0.0f});
			const float groundSize = 2.0f * kGroundHalfExtent;
			m = glm::scale(m, {groundSize, groundSize, 1.0f});
			if (m_groundEntity.IsValid())
			{
				world.Get<aether::TransformComponent>(m_groundEntity).localToWorld = m;
			}
		}

		// -- Fox autonomous wander ---------------------------------------------
		if (!m_foxInstances.empty())
		{
			std::uniform_real_distribution<float> posDist(-kGroundHalfExtent, kGroundHalfExtent);
			std::uniform_real_distribution<float> idleTimeDist(1.0f, 3.5f);
			std::uniform_int_distribution<int> decisionDist(0, 4); // 0-1 = pause, 2-4 = run again

			int runningCount = 0;

			for (FoxAgent& agent: m_foxAgents)
			{
				if (agent.idle)
				{
					agent.stateTimer -= dt;
					if (agent.stateTimer <= 0.0f)
					{
						// Done idling - pick a new target and start running.
						agent.target = {posDist(m_rng), 0.0f, posDist(m_rng)};
						agent.idle = false;
					}
				}
				else
				{
					const float dx = agent.target.x - agent.pos.x;
					const float dz = agent.target.z - agent.pos.z;
					const float dist = std::sqrt(dx * dx + dz * dz);

					if (dist < 0.8f)
					{
						// Arrived - randomly decide to pause or immediately pick a new target.
						if (decisionDist(m_rng) < 2)
						{
							agent.idle = true;
							agent.stateTimer = idleTimeDist(m_rng);
						}
						else
						{
							agent.target = {posDist(m_rng), 0.0f, posDist(m_rng)};
						}
					}
					else
					{
						// Smoothly rotate toward the target.
						const float desiredHeading = std::atan2(dx, dz);
						float diff = desiredHeading - agent.heading;
						while (diff > glm::pi<float>())
						{
							diff -= glm::two_pi<float>();
						}
						while (diff < -glm::pi<float>())
						{
							diff += glm::two_pi<float>();
						}

						const float turnSpeed = glm::radians(270.0f);
						agent.heading += std::clamp(diff, -turnSpeed * dt, turnSpeed * dt);

						// Move forward along the heading.
						agent.pos.x += std::sin(agent.heading) * kFoxRunSpeed * dt;
						agent.pos.z += std::cos(agent.heading) * kFoxRunSpeed * dt;
						agent.pos.x = std::clamp(agent.pos.x, -kGroundHalfExtent, kGroundHalfExtent);
						agent.pos.z = std::clamp(agent.pos.z, -kGroundHalfExtent, kGroundHalfExtent);
						++runningCount;
					}
				}
			}

			// Per-fox animation switch: each fox independently transitions between
			// Survey (idle) and Run based on its own state.
			for (std::size_t fi = 0; fi < m_foxAgents.size() && fi < m_foxInstances.size(); ++fi)
			{
				const std::uint32_t desired = m_foxAgents[fi].idle ? kAnimSurvey : kAnimRun;
				const float speed = m_foxAgents[fi].idle ? 1.0f : kFoxAnimRunSpeed;
				for (const aether::Entity e: m_foxInstances[fi])
				{
					if (auto* smc = world.TryGet<aether::SkinnedMeshComponent>(e))
					{
						if (smc->clipIndex != desired)
						{
							smc->clipIndex = desired;
							smc->playbackSpeed = speed;
							smc->animTime = 0.f;
						}
					}
				}
			}
			(void) runningCount;

			// Push the computed transform to every entity in each fox instance.
			for (std::size_t i = 0; i < m_foxInstances.size(); ++i)
			{
				const FoxAgent& agent = m_foxAgents[i];
				glm::mat4 foxMat = glm::translate(glm::mat4{1.0f}, agent.pos);
				foxMat = glm::rotate(foxMat, agent.heading, {0.0f, 1.0f, 0.0f});
				foxMat = glm::scale(foxMat, glm::vec3(0.05f));
				for (const aether::Entity e: m_foxInstances[i])
				{
					world.Get<aether::TransformComponent>(e).localToWorld = foxMat;
				}
			}
		}

		// -- Sky: centre cube spinning at kSkyHeight ---------------------------
		{
			glm::mat4 m = glm::translate(glm::mat4{1.0f}, {0.0f, kSkyHeight, 0.0f});
			m = glm::rotate(m, m_time * glm::radians(22.0f), {0.0f, 1.0f, 0.0f});
			m = glm::rotate(m, m_time * glm::radians(11.0f), {1.0f, 0.0f, 0.0f});
			m = glm::scale(m, {1.4f, 1.4f, 1.4f});
			if (m_centerEntity.IsValid())
			{
				world.Get<aether::TransformComponent>(m_centerEntity).localToWorld = m;
			}
		}

		// -- Sky: ring of cubes revolving at kSkyHeight ------------------------
		{
			constexpr float kRingRadius = 10.0f;
			const float kStep = glm::radians(360.0f / static_cast<float>(kRingCount));
			auto rView = world.View<RingComponent, aether::TransformComponent>();
			for (auto e: rView)
			{
				const int i = rView.get<RingComponent>(e).index;
				const float angle = m_time * glm::radians(40.0f) + static_cast<float>(i) * kStep;
				const glm::vec3 pos = {kRingRadius * std::cos(angle), kSkyHeight, kRingRadius * std::sin(angle)};
				const float selfSpin = m_time * glm::radians(90.0f + static_cast<float>(i) * 15.0f);

				glm::mat4 m = glm::translate(glm::mat4{1.0f}, pos);
				m = glm::rotate(m, selfSpin, {0.0f, 1.0f, 0.0f});
				m = glm::scale(m, {0.35f, 0.35f, 0.35f});
				rView.get<aether::TransformComponent>(e).localToWorld = m;
			}
		}

		// -- Sky: wide-orbit pair at kSkyHeight + 3 ----------------------------
		{
			const glm::vec3 diagAxis = glm::normalize(glm::vec3{1.0f, 1.0f, 0.3f});
			auto oView = world.View<OrbitComponent, aether::TransformComponent>();
			for (auto e: oView)
			{
				const float phase = oView.get<OrbitComponent>(e).phase;
				const float orbAngle = m_time * glm::radians(18.0f) + phase;
				const float bob = 0.8f * std::sin(m_time * 1.2f + phase);
				const glm::vec3 pos = {17.0f * std::cos(orbAngle), kSkyHeight + 3.0f + bob, 17.0f * std::sin(orbAngle)};

				glm::mat4 m = glm::translate(glm::mat4{1.0f}, pos);
				m = glm::rotate(m, m_time * glm::radians(55.0f), diagAxis);
				m = glm::scale(m, {0.8f, 0.8f, 0.8f});
				oView.get<aether::TransformComponent>(e).localToWorld = m;
			}
		}

		// -- Point-light markers: small cubes tinted to each light's color ----
		{
			auto markerView = world.View<PointLightMarkerComponent, aether::TransformComponent>();
			for (auto e: markerView)
			{
				const int idx = markerView.get<PointLightMarkerComponent>(e).index;
				if (idx < 0 || static_cast<std::size_t>(idx) >= m_pointLights.size())
				{
					continue;
				}

				// Test mode: drive both marker cubes and actual point-light sources
				// from the same animated position so any mismatch is impossible.
				const auto fi = static_cast<float>(idx);
				const float ring = 8.0f + static_cast<float>(idx % 16) * 4.0f;
				const float phase = fi * 0.37f;
				const float speed = 0.35f + static_cast<float>(idx % 5) * 0.09f;
				const float a = m_time * speed + phase;
				const glm::vec3 pos = {
				        ring * std::cos(a),
				        0.35f + 0.12f * std::sin(a * 1.7f + phase * 0.5f),
				        ring * std::sin(a),
				};

				m_pointLights[static_cast<std::size_t>(idx)].position = pos;
				glm::mat4 marker = glm::translate(glm::mat4{1.0f}, pos);
				marker = glm::scale(marker, glm::vec3(0.22f));
				markerView.get<aether::TransformComponent>(e).localToWorld = marker;
			}

			m_services->Get<Renderer>().SetPointLights(m_pointLights);
		}

		// -- Input: C = swap camera --------
		{
			if (m_input->IsKeyPressed(aether::Key::C))
			{
				const aether::CameraHandle active = m_cameras->GetMainCamera();
				const aether::CameraHandle next = (active == m_orbitCamera) ? m_freeCamera : m_orbitCamera;
				m_cameras->SetMainCamera(next);
				AE_INFO(aether::LogCategory::App, "Main camera: {}", (next == m_freeCamera) ? "Free" : "Orbit");
			}
		}

		// -- RTT camera slowly orbits looking down at the fox field ------------
		if (aether::Camera* cam = m_cameras->TryGet(m_rttCamera))
		{
			cam->SetOrbitYawPitch(m_time * 12.0f, 68.0f);
		}

		// -- Feed RTT output into the orbit cube -------------------------------
		const uint32_t rtSlot = m_services->Get<RenderTargetService>().GetRenderTargetBindlessSlot(m_rttTargetId);
		if (rtSlot != aether::Material::kNoTexture)
		{
			m_rttFeedMaterial.albedoSlot = rtSlot;
			m_assets->RegisterMaterial(m_rttFeedMaterial);
			auto rttView = world.View<OrbitComponent, aether::MaterialComponent>();
			for (auto e: rttView)
			{
				if (rttView.get<OrbitComponent>(e).isRttTarget)
				{
					rttView.get<aether::MaterialComponent>(e).material = m_rttFeedMaterial;
					break;
				}
			}
		}
	}

	void SandboxGameSystem::OnUnregister(aether::World& world)
	{
		if (!m_services || !m_assets || !m_cameras)
		{
			return;
		}

		AE_INFO(aether::LogCategory::App, "SandboxGameSystem unregistered.");

		// Destroy all sandbox entities in one pass.
		for (const aether::Entity e: m_sandboxEntities)
		{
			if (e.IsValid())
			{
				world.Destroy(e);
			}
		}
		m_sandboxEntities.clear();

		m_effectManager.DestroyAll(*m_assets);

		m_foxAgents.clear();
		m_foxInstances.clear();

		// Unregister fox model materials.
		if (m_foxModel)
		{
			for (aether::LoadedModelPrimitive& primitive: m_foxModel->primitives)
			{
				m_assets->UnregisterMaterial(primitive.material);
			}
		}
		m_foxModel.reset();

		m_assets->UnregisterMaterial(m_rttFeedMaterial);
		m_assets->UnregisterMaterial(m_untexturedMaterial);
		m_assets->UnregisterMaterial(m_debugTexturedMaterial);
		for (aether::Material& markerMaterial: m_pointLightMarkerMaterials)
		{
			m_assets->UnregisterMaterial(markerMaterial);
		}
		m_pointLightMarkerMaterials.clear();
		m_pointLights.clear();

		m_services->Get<RenderTargetService>().DestroyCameraRenderTarget(m_rttTargetId);
		m_rttTargetId = 0;

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

		m_services->Get<Renderer>().ClearPointLights();
		m_services->Get<Renderer>().ClearSpotLights();

		for (aether::Texture& texture: m_debugMaterialTextures)
		{
			texture.Destroy();
		}
		m_debugMaterialTextures.clear();
		m_planeMesh = nullptr;
		m_quadMesh = nullptr;
		m_cubeMesh = nullptr;
		m_pipeline.Destroy();
	}
} // namespace aether::app
