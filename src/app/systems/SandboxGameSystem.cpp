#include "SandboxGameSystem.hpp"

#include <array>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "AetherCore.hpp"
#include "AssetManager.hpp"
#include "Camera.hpp"
#include "EcsHelpers.hpp"
#include "FileSystem.hpp"
#include "Input.hpp"
#include "Logger.hpp"
#include "World.hpp"

namespace aether::app
{
	namespace
	{
		struct SandboxEntityTag
		{
			bool value = true;
		};

		struct GroundTag
		{
			bool value = true;
		};

		struct CenterTag
		{
			bool value = true;
		};

		struct RingTag
		{
			int index = 0;
		};

		struct OrbitTag
		{
			float phase = 0.0f;
			bool isRttTarget = false;
		};

		struct FoxTag
		{
			bool value = true;
		};

		// Identifies which fox instance (index into m_foxAgents) this entity belongs to.
		struct FoxInstanceIndex
		{
			int index = 0;
		};
	} // namespace

	std::uint32_t SandboxGameSystem::GetAnimationCount() const
	{
		if (m_foxAnimators.empty())
			return 0;
		return m_foxAnimators.front().GetAnimationCount();
	}

	std::string_view SandboxGameSystem::GetCurrentAnimationName() const
	{
		if (m_foxAnimators.empty())
			return {};
		const aether::ModelAnimator& first = m_foxAnimators.front();
		return first.GetAnimationName(first.GetCurrentAnimation());
	}

	void SandboxGameSystem::Init(aether::AetherCore& engine, aether::AssetManager& assets, aether::CameraManager& cameras, aether::Input& input)
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
		const VkDescriptorSetLayout bindlessLayout = m_engine->GetBindlessManager().GetLayout();
		const VkDescriptorSetLayout lightingLayout = m_engine->GetLightingSetLayout();
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

		// ── Ground: large untextured quad ─────────────────────────────────────
		{
			const aether::Entity e = aether::ecs::SpawnMesh(world, m_pipeline, *m_quadMesh, m_untexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(e, SandboxEntityTag{});
			world.EmplaceOrReplace<GroundTag>(e, GroundTag{});
		}

		// ── Foxes ─────────────────────────────────────────────────────────────
		constexpr std::string_view kFoxPath = "assets://models/Fox/Fox.gltf";
		if (aether::io::FileSystem::Exists(kFoxPath))
		{
			m_foxModel = m_assets->LoadModel(kFoxPath);
			if (m_foxModel->animator)
			{
				const std::uint32_t animCount = m_foxModel->animator->GetAnimationCount();
				INFO(aether::LogCategory::App, "Fox glTF has {} animation(s):", animCount);
				for (std::uint32_t i = 0; i < animCount; ++i)
					INFO(aether::LogCategory::App, "  [{}] {}", i, m_foxModel->animator->GetAnimationName(i));
			}

			std::uniform_real_distribution<float> posDist(-kGroundHalfExtent, kGroundHalfExtent);
			std::uniform_real_distribution<float> angleDist(0.0f, glm::two_pi<float>());
			std::uniform_real_distribution<float> timerDist(0.2f, 2.0f);

			m_foxAgents.reserve(kFoxCount);
			m_foxInstances.reserve(kFoxCount);
			m_foxAnimators.reserve(kFoxCount);

			for (int i = 0; i < kFoxCount; ++i)
			{
				// Spawn all primitives of this fox instance (shared mesh/skin, independent transform).
				const std::vector<aether::Entity> foxEntities = m_assets->SpawnModel(*m_foxModel, m_pipeline, 0.05f);

				FoxAgent agent;
				agent.pos = { posDist(m_rng), 0.0f, posDist(m_rng) };
				agent.heading = angleDist(m_rng);
				agent.target = { posDist(m_rng), 0.0f, posDist(m_rng) };
				agent.stateTimer = timerDist(m_rng);
				agent.idle = false;
				m_foxAgents.push_back(agent);

				// Clone a fully-independent animator for this fox instance.
				if (m_foxModel->animator)
				{
					m_foxAnimators.push_back(m_foxModel->animator->Clone());
					aether::ModelAnimator& foxAnim = m_foxAnimators.back();
					foxAnim.SetAnimation(kAnimRun);
					foxAnim.SetPlaybackSpeed(kFoxAnimRunSpeed);
					// Stagger each fox's animation phase so they don't move in lockstep.
					if (foxAnim.GetDuration() > 0.0f)
					{
						std::uniform_real_distribution<float> phaseDist(0.0f, foxAnim.GetDuration());
						foxAnim.SetAnimTime(phaseDist(m_rng));
					}
				}

				std::vector<aether::Entity> instances;
				instances.reserve(foxEntities.size());
				for (const aether::Entity e: foxEntities)
				{
					world.EmplaceOrReplace<SandboxEntityTag>(e, SandboxEntityTag{});
					world.EmplaceOrReplace<FoxTag>(e, FoxTag{});
					world.EmplaceOrReplace<FoxInstanceIndex>(e, FoxInstanceIndex{ i });
					instances.push_back(e);
				}

				// Link each primitive entity to this fox's own animator and skin buffer.
				if (!m_foxAnimators.empty() && static_cast<int>(m_foxAnimators.size()) == i + 1)
				{
					aether::ModelAnimator& foxAnim = m_foxAnimators.back();
					for (std::size_t p = 0; p < instances.size() && p < m_foxModel->primitives.size(); ++p)
					{
						const aether::Entity e = instances[p];
						const std::int32_t skinIdx = m_foxModel->primitives[p].skinIndex;
						if (skinIdx >= 0)
						{
							const VkDeviceAddress addr = foxAnim.GetSkinBufferAddr(skinIdx);
							if (addr != 0)
								world.EmplaceOrReplace<aether::SkinComponent>(e, aether::SkinComponent{ .skinBufferAddr = addr });
						}
						world.EmplaceOrReplace<aether::AnimatorComponent>(e, aether::AnimatorComponent{ .animator = &foxAnim });
					}
				}

				m_foxInstances.push_back(std::move(instances));
			}
			INFO(aether::LogCategory::App, "Spawned {} fox instances.", kFoxCount);
		}
		else
		{
			INFO(aether::LogCategory::App, "Fox model not found at '{}'; skipping.", kFoxPath);
		}

		// ── Sky cubes ─────────────────────────────────────────────────────────
		// Centre spinning cube
		{
			const aether::Entity e = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_debugTexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(e, SandboxEntityTag{});
			world.EmplaceOrReplace<CenterTag>(e, CenterTag{});
		}

		// Ring of 8 small cubes
		for (int i = 0; i < kRingCount; ++i)
		{
			const aether::Entity e = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_untexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(e, SandboxEntityTag{});
			world.EmplaceOrReplace<RingTag>(e, RingTag{ .index = i });
		}

		// Wide-orbit pair — one textured, one RTT-fed
		{
			const aether::Entity eA = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_debugTexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(eA, SandboxEntityTag{});
			world.EmplaceOrReplace<OrbitTag>(eA, OrbitTag{ .phase = 0.0f, .isRttTarget = false });

			const aether::Entity eB = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_untexturedMaterial);
			world.EmplaceOrReplace<SandboxEntityTag>(eB, SandboxEntityTag{});
			world.EmplaceOrReplace<OrbitTag>(eB, OrbitTag{ .phase = glm::radians(180.0f), .isRttTarget = true });
		}

		// ── Cameras ──────────────────────────────────────────────────────────
		// Main orbit: pulled back far enough to see the entire fox field.
		m_orbitCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Orbit,
		        .orbitTarget = { 0.0f, 0.0f, 0.0f },
		        .orbitDistance = 48.0f,
		        .orbitYaw = 35.0f,
		        .orbitPitch = 28.0f,
		});

		m_freeCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Free,
		        .position = { 0.0f, 5.0f, 46.0f },
		        .yaw = 0.0f,
		        .pitch = -8.0f,
		        .moveSpeed = 12.0f,
		        .lookSpeed = 0.14f,
		});

		// RTT camera: top-down-ish view orbiting above the fox field.
		m_rttCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Orbit,
		        .orbitTarget = { 0.0f, 0.0f, 0.0f },
		        .orbitDistance = 40.0f,
		        .orbitYaw = 0.0f,
		        .orbitPitch = 68.0f,
		});

		m_cameras->SetMainCamera(m_orbitCamera);
		m_rttTarget = m_engine->CreateCameraRenderTarget(m_rttCamera, { 512, 512 });

		// ── Point lights scattered across the fox field ───────────────────────
		{
			std::mt19937 lightRng(7);
			std::uniform_real_distribution<float> lightPosDist(-22.0f, 22.0f);
			std::uniform_real_distribution<float> colorDist(0.35f, 1.0f);

			std::vector<aether::Renderer::PointLight> pointLights;
			pointLights.reserve(16);
			for (int li = 0; li < 16; ++li)
			{
				aether::Renderer::PointLight l{};
				l.position = { lightPosDist(lightRng), 1.5f, lightPosDist(lightRng) };
				l.radius = 14.0f;
				l.intensity = 1.8f;
				l.color = { colorDist(lightRng), colorDist(lightRng), colorDist(lightRng) };
				pointLights.push_back(l);
			}
			m_engine->GetRenderer().SetPointLights(std::move(pointLights));
		}

		INFO(aether::LogCategory::App, "Scene built: {} foxes on ground + {} ring + 1 center + 2 orbit sky cubes.", kFoxCount, kRingCount);
	}

	void SandboxGameSystem::Update(aether::World& world, float dt)
	{
		if (!m_engine || !m_cameras || !m_input)
			return;

		m_time += dt;

		// ── Ground: large flat quad, 56×56 units, lying at Y=0 ───────────────
		{
			glm::mat4 m = glm::rotate(glm::mat4{ 1.0f }, glm::radians(-90.0f), { 1.0f, 0.0f, 0.0f });
			m = glm::scale(m, { 56.0f, 56.0f, 1.0f });
			auto gView = world.View<GroundTag, aether::TransformComponent>();
			for (auto e: gView)
				gView.get<aether::TransformComponent>(e).localToWorld = m;
		}

		// ── Fox autonomous wander ─────────────────────────────────────────────
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
						// Done idling — pick a new target and start running.
						agent.target = { posDist(m_rng), 0.0f, posDist(m_rng) };
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
						// Arrived — randomly decide to pause or immediately pick a new target.
						if (decisionDist(m_rng) < 2)
						{
							agent.idle = true;
							agent.stateTimer = idleTimeDist(m_rng);
						}
						else
						{
							agent.target = { posDist(m_rng), 0.0f, posDist(m_rng) };
						}
					}
					else
					{
						// Smoothly rotate toward the target.
						const float desiredHeading = std::atan2(dx, dz);
						float diff = desiredHeading - agent.heading;
						while (diff > glm::pi<float>())
							diff -= glm::two_pi<float>();
						while (diff < -glm::pi<float>())
							diff += glm::two_pi<float>();

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
			for (std::size_t fi = 0; fi < m_foxAgents.size() && fi < m_foxAnimators.size(); ++fi)
			{
				aether::ModelAnimator& foxAnim = m_foxAnimators[fi];
				const std::uint32_t desired = m_foxAgents[fi].idle ? kAnimSurvey : kAnimRun;
				if (foxAnim.GetCurrentAnimation() != desired)
				{
					foxAnim.SetAnimation(desired);
					foxAnim.SetPlaybackSpeed(m_foxAgents[fi].idle ? 1.0f : kFoxAnimRunSpeed);
				}
			}
			(void) runningCount;

			// Push the computed transform to every entity in each fox instance.
			for (std::size_t i = 0; i < m_foxInstances.size(); ++i)
			{
				const FoxAgent& agent = m_foxAgents[i];
				glm::mat4 foxMat = glm::translate(glm::mat4{ 1.0f }, agent.pos);
				foxMat = glm::rotate(foxMat, agent.heading, { 0.0f, 1.0f, 0.0f });
				foxMat = glm::scale(foxMat, glm::vec3(0.05f));
				for (const aether::Entity e: m_foxInstances[i])
					world.Get<aether::TransformComponent>(e).localToWorld = foxMat;
			}
		}

		// ── Sky: centre cube spinning at kSkyHeight ───────────────────────────
		{
			glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, { 0.0f, kSkyHeight, 0.0f });
			m = glm::rotate(m, m_time * glm::radians(22.0f), { 0.0f, 1.0f, 0.0f });
			m = glm::rotate(m, m_time * glm::radians(11.0f), { 1.0f, 0.0f, 0.0f });
			m = glm::scale(m, { 1.4f, 1.4f, 1.4f });
			auto cView = world.View<CenterTag, aether::TransformComponent>();
			for (auto e: cView)
				cView.get<aether::TransformComponent>(e).localToWorld = m;
		}

		// ── Sky: ring of cubes revolving at kSkyHeight ────────────────────────
		{
			constexpr float kRingRadius = 10.0f;
			const float kStep = glm::radians(360.0f / static_cast<float>(kRingCount));
			auto rView = world.View<RingTag, aether::TransformComponent>();
			for (auto e: rView)
			{
				const int i = rView.get<RingTag>(e).index;
				const float angle = m_time * glm::radians(40.0f) + static_cast<float>(i) * kStep;
				const glm::vec3 pos = { kRingRadius * std::cos(angle), kSkyHeight, kRingRadius * std::sin(angle) };
				const float selfSpin = m_time * glm::radians(90.0f + static_cast<float>(i) * 15.0f);

				glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, pos);
				m = glm::rotate(m, selfSpin, { 0.0f, 1.0f, 0.0f });
				m = glm::scale(m, { 0.35f, 0.35f, 0.35f });
				rView.get<aether::TransformComponent>(e).localToWorld = m;
			}
		}

		// ── Sky: wide-orbit pair at kSkyHeight + 3 ────────────────────────────
		{
			const glm::vec3 diagAxis = glm::normalize(glm::vec3{ 1.0f, 1.0f, 0.3f });
			auto oView = world.View<OrbitTag, aether::TransformComponent>();
			for (auto e: oView)
			{
				const float phase = oView.get<OrbitTag>(e).phase;
				const float orbAngle = m_time * glm::radians(18.0f) + phase;
				const float bob = 0.8f * std::sin(m_time * 1.2f + phase);
				const glm::vec3 pos = { 17.0f * std::cos(orbAngle), kSkyHeight + 3.0f + bob, 17.0f * std::sin(orbAngle) };

				glm::mat4 m = glm::translate(glm::mat4{ 1.0f }, pos);
				m = glm::rotate(m, m_time * glm::radians(55.0f), diagAxis);
				m = glm::scale(m, { 0.8f, 0.8f, 0.8f });
				oView.get<aether::TransformComponent>(e).localToWorld = m;
			}
		}

		// ── Input: T = cycle tonemap, F = toggle FXAA, C = swap camera ────────
		{
			if (m_input->IsKeyPressed(aether::Key::T))
			{
				const auto next = static_cast<aether::TonemapMode>((static_cast<int>(m_engine->GetTonemapMode()) + 1) % 3);
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

			if (m_input->IsKeyPressed(aether::Key::C))
			{
				const aether::CameraHandle active = m_cameras->GetMainCamera();
				const aether::CameraHandle next = (active == m_orbitCamera) ? m_freeCamera : m_orbitCamera;
				m_cameras->SetMainCamera(next);
				INFO(aether::LogCategory::App, "Main camera: {}", (next == m_freeCamera) ? "Free" : "Orbit");
			}
		}

		// ── RTT camera slowly orbits looking down at the fox field ────────────
		if (aether::Camera* cam = m_cameras->TryGet(m_rttCamera))
		{
			cam->SetOrbitYawPitch(m_time * 12.0f, 68.0f);
		}

		// ── Feed RTT output into the orbit cube ───────────────────────────────
		const uint32_t rtSlot = m_engine->GetRenderTargetBindlessSlot(m_rttTarget);
		if (rtSlot != aether::Material::kNoTexture)
		{
			m_rttFeedMaterial.albedoSlot = rtSlot;
			m_assets->RegisterMaterial(m_rttFeedMaterial);
			auto rttView = world.View<OrbitTag, aether::MaterialComponent>();
			for (auto e: rttView)
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

		// Destroy all sandbox entities in one pass.
		std::vector<entt::entity> toDestroy;
		{
			auto allView = world.View<SandboxEntityTag>();
			toDestroy.assign(allView.begin(), allView.end());
		}
		for (const entt::entity e: toDestroy)
			world.Destroy(aether::Entity{ static_cast<std::uint32_t>(entt::to_integral(e)) });

		m_foxAgents.clear();
		m_foxInstances.clear();
		m_foxAnimators.clear();

		// Unregister fox model materials.
		if (m_foxModel)
		{
			for (aether::LoadedModelPrimitive& primitive: m_foxModel->primitives)
				m_assets->UnregisterMaterial(primitive.material);
		}
		m_foxModel.reset();

		m_assets->UnregisterMaterial(m_rttFeedMaterial);
		m_assets->UnregisterMaterial(m_untexturedMaterial);
		m_assets->UnregisterMaterial(m_debugTexturedMaterial);

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

		m_debugTexture.Destroy();
		m_cubeMesh = nullptr;
		m_quadMesh = nullptr;
		m_pipeline.Destroy();
	}
} // namespace aether::app
