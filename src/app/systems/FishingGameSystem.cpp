#include "FishingGameSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "camera/Camera.hpp"
#include "rendering/LightingManager.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/EcsHelpers.hpp"
#include "io/FileSystem.hpp"
#include "platform/Input.hpp"
#include "assets/AssetManager.hpp"
#include "utils/Logger.hpp"
#include "gpu/BindlessManager.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/Window.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include <GLFW/glfw3.h>
#include <glm/gtx/quaternion.hpp>

namespace aether::app
{
	namespace
	{
		constexpr float kWaterLevel = 0.0f;
		constexpr float kLakeEdgeRadius = FishingGameSystem::kLakeRadius - 2.5f;
		constexpr float kFishTurnSpeed = glm::radians(120.0f);
		constexpr float kFishWanderRadius = FishingGameSystem::kLakeRadius - 4.0f;
		constexpr float kFishBiteDistance = 2.0f;
		constexpr float kFishBobSpeed = 2.2f;

		glm::vec3 Unproject(const glm::vec3& ndc, const glm::mat4& invViewProj)
		{
			const glm::vec4 worldPos = invViewProj * glm::vec4(ndc, 1.0f);
			return glm::vec3(worldPos) / worldPos.w;
		}

		bool ScreenPointToWorldRay(const Camera& cam, const glm::vec2& mousePx, const glm::vec2& frameSize, glm::vec3& outOrigin, glm::vec3& outDirection)
		{
			if (frameSize.x <= 0.0f || frameSize.y <= 0.0f)
			{
				return false;
			}

			const float aspect = frameSize.x / frameSize.y;
			const glm::mat4 invViewProj = glm::inverse(cam.GetViewProjectionMatrix(aspect));
			const glm::vec2 ndc = glm::vec2((mousePx.x / frameSize.x) * 2.0f - 1.0f, 1.0f - (mousePx.y / frameSize.y) * 2.0f);
			const glm::vec3 nearPoint = Unproject(glm::vec3(ndc, -1.0f), invViewProj);
			const glm::vec3 farPoint = Unproject(glm::vec3(ndc, 1.0f), invViewProj);
			outOrigin = nearPoint;
			outDirection = glm::normalize(farPoint - nearPoint);
			return true;
		}

		bool RayPlaneIntersect(const glm::vec3& origin, const glm::vec3& direction, float planeY, glm::vec3& outPoint)
		{
			if (std::abs(direction.y) < 1e-5f)
			{
				return false;
			}

			const float t = (planeY - origin.y) / direction.y;
			if (t <= 0.0f)
			{
				return false;
			}

			outPoint = origin + direction * t;
			return true;
		}

		glm::mat4 MakeLineTransform(const glm::vec3& start, const glm::vec3& end, float thickness)
		{
			const glm::vec3 delta = end - start;
			const float length = glm::length(delta);
			if (length < 1e-5f)
			{
				return glm::translate(glm::mat4{1.0f}, start);
			}

			const glm::vec3 direction = delta / length;
			const glm::vec3 zAxis = glm::vec3{0.0f, 0.0f, 1.0f};
			const glm::quat rotation = glm::rotation(zAxis, direction);
			glm::mat4 transform = glm::translate(glm::mat4{1.0f}, start + delta * 0.5f);
			transform = transform * glm::mat4_cast(rotation);
			transform = glm::scale(transform, glm::vec3{thickness, thickness, length});
			return transform;
		}

		const char* BobberStateName(FishingGameSystem::BobberState state)
		{
			switch (state)
			{
				case FishingGameSystem::BobberState::Ready:
					return "Ready";
				case FishingGameSystem::BobberState::Casting:
					return "Casting";
				case FishingGameSystem::BobberState::Waiting:
					return "Waiting";
				case FishingGameSystem::BobberState::Biting:
					return "Biting";
				case FishingGameSystem::BobberState::Hooked:
					return "Hooked";
				default:
					return "Unknown";
			}
		}
	} // namespace

	const char* FishingGameSystem::GetBobberStateName() const
	{
		return BobberStateName(m_bobberState);
	}

	void FishingGameSystem::Init(ServiceContainer& services, aether::AssetManager& assets, aether::CameraManager& cameras, aether::Input& input)
	{
		m_services = &services;
		m_assets = &assets;
		m_cameras = &cameras;
		m_input = &input;
	}

	void FishingGameSystem::UpdatePlayer(float dt)
	{
		const bool rotate = m_input->IsMouseButtonDown(aether::MouseButton::Right);
		if (rotate)
		{
			const glm::vec2 delta = m_input->GetMouseDelta();
			m_player.yaw -= delta.x * m_player.lookSpeed;
			m_player.pitch -= delta.y * m_player.lookSpeed;
			m_player.pitch = std::clamp(m_player.pitch, -89.0f, 89.0f);
		}

		const float yawRad = glm::radians(m_player.yaw);
		const glm::vec3 forward = glm::normalize(glm::vec3{-std::sin(yawRad), 0.0f, -std::cos(yawRad)});
		const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}));

		if (m_input->IsKeyDown(aether::Key::W))
		{
			m_player.position += forward * m_player.moveSpeed * dt;
		}
		if (m_input->IsKeyDown(aether::Key::S))
		{
			m_player.position -= forward * m_player.moveSpeed * dt;
		}
		if (m_input->IsKeyDown(aether::Key::D))
		{
			m_player.position += right * m_player.moveSpeed * dt;
		}
		if (m_input->IsKeyDown(aether::Key::A))
		{
			m_player.position -= right * m_player.moveSpeed * dt;
		}

		if (aether::Camera* camera = m_cameras->TryGet(m_playerCamera))
		{
			camera->SetPosition(m_player.position + glm::vec3{0.0f, m_player.headHeight, 0.0f});
			camera->SetYawPitch(m_player.yaw, m_player.pitch);
		}
	}

	bool FishingGameSystem::TryGetWaterHitPoint(glm::vec3& outTarget) const
	{
		if (!m_services || !m_input)
		{
			return false;
		}

		aether::Camera* camera = m_cameras->TryGet(m_playerCamera);
		if (!camera)
		{
			return false;
		}

		GLFWwindow* handle = m_services->Get<Window>().GetHandle();
		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(handle, &width, &height);
		glm::vec2 frameSize{static_cast<float>(width), static_cast<float>(height)};

		glm::vec2 mousePos = m_input->GetMousePos();
		glm::vec3 rayOrigin;
		glm::vec3 rayDirection;
		if (!ScreenPointToWorldRay(*camera, mousePos, frameSize, rayOrigin, rayDirection))
		{
			return false;
		}

		if (!RayPlaneIntersect(rayOrigin, rayDirection, kWaterLevel, outTarget))
		{
			return false;
		}

		const glm::vec2 flat = glm::vec2(outTarget.x, outTarget.z);
		if (glm::length(flat) > kLakeRadius)
		{
			const glm::vec2 clamped = glm::normalize(flat) * kLakeRadius;
			outTarget = glm::vec3(clamped.x, kWaterLevel, clamped.y);
		}

		return true;
	}

	glm::vec3 FishingGameSystem::GetRodWorldOrigin() const
	{
		aether::Camera* camera = m_cameras->TryGet(m_playerCamera);
		if (!camera)
		{
			return m_player.position + glm::vec3{0.0f, m_player.headHeight, 0.0f};
		}

		const glm::vec3 forward = glm::normalize(camera->GetForward());
		return camera->GetPosition() + forward * 0.5f + glm::vec3{0.0f, -0.18f, 0.0f};
	}

	void FishingGameSystem::OnRegister(aether::World& world)
	{
		AE_INFO(aether::LogCategory::App, "FishingGameSystem registered.");
		if (!m_services || !m_assets || !m_cameras || !m_input)
		{
			AE_WARN(aether::LogCategory::App, "FishingGameSystem not initialized with dependencies!");
			return;
		}

		const aether::gpu::DescriptorSetLayout bindlessLayout = m_services->Get<BindlessManager>().GetLayout();
		const aether::gpu::DescriptorSetLayout lightingLayout = m_services->Get<LightingManager>().GetSetLayout();
		const std::array<aether::gpu::DescriptorSetLayout, 2> setLayouts{bindlessLayout, lightingLayout};

		AE_EXPECT_OR_THROW(pipeline,
		        m_services->Get<AssetManager>().CreateGraphicsPipeline({
		                .shaderVfsPath = "shaders://gltf_mesh.spv",
		                .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		                .depthFormat = m_services->Get<Swapchain>().GetDepthFormat(),
		                .depthTestEnable = true,
		                .depthWriteEnable = true,
		                .setLayouts = std::span<const aether::gpu::DescriptorSetLayout>(setLayouts.data(), setLayouts.size()),
		        }));
		m_pipeline = std::move(pipeline);

		m_planeMesh = &m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Plane);
		m_cubeMesh = &m_services->Get<PrimitiveMeshes>().Get(aether::PrimitiveMesh::Cube);

		m_waterMaterial = {};
		m_waterMaterial.baseColorFactor = glm::vec4(0.14f, 0.42f, 0.72f, 0.90f);
		m_waterMaterial.roughnessFactor = 0.18f;
		m_waterMaterial.metallicFactor = 0.0f;
		m_waterMaterial.emissiveFactor = glm::vec3(0.08f);
		m_assets->RegisterMaterial(m_waterMaterial);

		m_shoreMaterial = {};
		m_shoreMaterial.baseColorFactor = glm::vec4(0.32f, 0.22f, 0.12f, 1.0f);
		m_shoreMaterial.roughnessFactor = 0.9f;
		m_assets->RegisterMaterial(m_shoreMaterial);

		m_fishMaterial = {};
		m_fishMaterial.baseColorFactor = glm::vec4(0.92f, 0.54f, 0.16f, 1.0f);
		m_fishMaterial.roughnessFactor = 0.5f;
		m_assets->RegisterMaterial(m_fishMaterial);

		m_bobberMaterial = {};
		m_bobberMaterial.baseColorFactor = glm::vec4(0.94f, 0.12f, 0.12f, 1.0f);
		m_bobberMaterial.emissiveFactor = glm::vec3(0.12f);
		m_assets->RegisterMaterial(m_bobberMaterial);

		// Water surface.
		{
			const glm::mat4 transform = glm::scale(glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), glm::vec3{1.0f, 0.0f, 0.0f}), glm::vec3{kWaterSize, kWaterSize, 1.0f});
			m_waterEntity = aether::ecs::SpawnMesh(world, m_pipeline, *m_planeMesh, m_waterMaterial, transform);
			world.EmplaceOrReplace<FishingEntityTag>(m_waterEntity, FishingEntityTag{});
		}

		// Fish population.
		std::uniform_real_distribution<float> radiusDist(4.0f, kFishWanderRadius);
		std::uniform_real_distribution<float> angleDist(0.0f, glm::two_pi<float>());
		std::uniform_real_distribution<float> speedDist(kFishSpeedMin, kFishSpeedMax);

		m_fishEntities.reserve(kFishCount);
		m_fishAgents.reserve(kFishCount);

		for (int i = 0; i < kFishCount; ++i)
		{
			const float radius = radiusDist(m_rng);
			const float angle = angleDist(m_rng);
			const glm::vec3 position = glm::vec3(std::cos(angle) * radius, kWaterLevel, std::sin(angle) * radius);
			const glm::vec3 target = glm::vec3(std::cos(angle + 1.2f) * radius, kWaterLevel, std::sin(angle + 1.2f) * radius);

			FishAgent agent;
			agent.pos = position;
			agent.target = target;
			agent.heading = angle + glm::half_pi<float>();
			agent.speed = speedDist(m_rng);
			agent.bobPhase = angleDist(m_rng);
			agent.hooked = false;
			m_fishAgents.push_back(agent);

			const aether::Entity entity = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_fishMaterial);
			world.EmplaceOrReplace<FishingEntityTag>(entity, FishingEntityTag{});
			m_fishEntities.push_back(entity);

			glm::mat4 fishXf = glm::translate(glm::mat4{1.0f}, agent.pos);
			fishXf = glm::rotate(fishXf, agent.heading, glm::vec3{0.0f, 1.0f, 0.0f});
			fishXf = glm::scale(fishXf, glm::vec3{kFishScale});
			world.EmplaceOrReplace<aether::TransformComponent>(entity, aether::TransformComponent{.localToWorld = fishXf});
		}

		// Bobber.
		{
			m_bobberPos = glm::vec3(0.0f, kWaterLevel + kBobberHeight, 0.0f);
			const glm::mat4 bobberXf = glm::scale(glm::translate(glm::mat4{1.0f}, m_bobberPos), glm::vec3{kBobberScale});
			m_bobberEntity = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_bobberMaterial, bobberXf);
			world.EmplaceOrReplace<FishingEntityTag>(m_bobberEntity, FishingEntityTag{});
		}

		m_lineMaterial = {};
		m_lineMaterial.baseColorFactor = glm::vec4(0.94f, 0.94f, 0.80f, 1.0f);
		m_lineMaterial.emissiveFactor = glm::vec3(0.22f);
		m_lineMaterial.roughnessFactor = 0.35f;
		m_assets->RegisterMaterial(m_lineMaterial);

		// Fishing line.
		{
			const glm::mat4 lineXf = glm::scale(glm::translate(glm::mat4{1.0f}, GetRodWorldOrigin()), glm::vec3{0.0f});
			m_lineEntity = aether::ecs::SpawnMesh(world, m_pipeline, *m_cubeMesh, m_lineMaterial, lineXf);
			world.EmplaceOrReplace<FishingEntityTag>(m_lineEntity, FishingEntityTag{});
		}

		m_playerCamera = m_cameras->Create({
		        .mode = aether::CameraMode::Manual,
		        .position = m_player.position,
		        .yaw = m_player.yaw,
		        .pitch = m_player.pitch,
		        .moveSpeed = m_player.moveSpeed,
		        .lookSpeed = m_player.lookSpeed,
		});
		m_cameras->SetMainCamera(m_playerCamera);

		m_bobberPos = GetRodWorldOrigin();

		m_services->Get<Renderer>().SetDirectionalLight(glm::normalize(glm::vec3{0.4f, -1.0f, 0.25f}), 2.2f);
		m_services->Get<Renderer>().SetAmbientLight(glm::vec3{0.22f, 0.28f, 0.35f});
	}

	void FishingGameSystem::Update(aether::World& world, float dt)
	{
		if (!m_services || !m_cameras || !m_input)
		{
			return;
		}

		m_time += dt;
		UpdatePlayer(dt);
		aether::Camera* camera = m_cameras->TryGet(m_playerCamera);
		if (!camera)
		{
			return;
		}

		const glm::vec3 cameraPos = camera->GetPosition();
		const glm::vec3 cameraForward = glm::normalize(camera->GetForward());

		if (m_bobberState == BobberState::Ready)
		{
			if (m_input->IsMouseButtonPressed(aether::MouseButton::Left))
			{
				glm::vec3 target;
				if (TryGetWaterHitPoint(target))
				{
					m_bobberState = BobberState::Casting;
					m_castOrigin = GetRodWorldOrigin();
					m_bobberPos = m_castOrigin;
					m_bobberTarget = target;
					m_castDirection = glm::normalize(m_bobberTarget - m_castOrigin);
					m_castTotalDistance = glm::distance(m_castOrigin, m_bobberTarget);
					m_castProgress = 0.0f;
				}
			}
		}
		else if (m_bobberState == BobberState::Casting)
		{
			m_castProgress += dt * kCastSpeed;
			const float tRaw = m_castTotalDistance > 0.0f ? m_castProgress / m_castTotalDistance : 1.0f;
			const float t = std::min(tRaw, 1.0f);
			const float ease = 1.0f - std::pow(1.0f - t, 2.2f);
			glm::vec3 base = glm::mix(m_castOrigin, m_bobberTarget, ease);
			const float distanceFactor = glm::clamp(m_castTotalDistance / kMaxCastDistance, 0.0f, 1.0f);
			const float maxArc = kCastArcBase + distanceFactor * kCastArcScale;
			const float arcShape = glm::sin(glm::pi<float>() * t);
			const float arc = maxArc * arcShape;
			m_bobberPos = base + glm::vec3{0.0f, arc, 0.0f};
			if (t >= 1.0f || glm::length(m_bobberPos - m_bobberTarget) < 0.2f)
			{
				m_bobberPos = m_bobberTarget + glm::vec3{0.0f, kBobberHeight, 0.0f};
				m_bobberState = BobberState::Waiting;
				std::uniform_real_distribution<float> waitDist(1.2f, 3.2f);
				m_waitTimer = waitDist(m_rng);
				m_biteTimer = 0.0f;
				m_hookedFishIndex = -1;
			}
		}
		else if (m_bobberState == BobberState::Waiting)
		{
			m_bobberPos.y = kWaterLevel + kBobberHeight + std::sin(m_time * 1.8f) * 0.06f;
			m_waitTimer -= dt;

			if (m_waitTimer <= 0.0f)
			{
				std::uniform_int_distribution<int> indexDist(0, static_cast<int>(m_fishAgents.size()) - 1);
				m_hookedFishIndex = indexDist(m_rng);
				m_bobberState = BobberState::Biting;
				m_biteTimer = kBiteHoldTime;
			}
		}
		else if (m_bobberState == BobberState::Biting)
		{
			m_bobberPos.y = kWaterLevel + kBobberHeight + std::sin(m_time * 10.0f) * 0.14f;
			m_biteTimer -= dt;
			if (m_input->IsKeyPressed(aether::Key::Space))
			{
				m_bobberState = BobberState::Hooked;
				if (m_hookedFishIndex >= 0 && m_hookedFishIndex < static_cast<int>(m_fishAgents.size()))
				{
					m_fishAgents[m_hookedFishIndex].hooked = true;
				}
			}
			else if (m_biteTimer <= 0.0f)
			{
				m_bobberState = BobberState::Waiting;
				std::uniform_real_distribution<float> waitDist(1.2f, 3.2f);
				m_waitTimer = waitDist(m_rng);
				m_hookedFishIndex = -1;
			}
		}
		else if (m_bobberState == BobberState::Hooked)
		{
			const glm::vec3 rodOrigin = GetRodWorldOrigin();
			const glm::vec3 toRod = rodOrigin - m_bobberPos;
			const float distance = glm::length(toRod);
			const bool reeling = m_input->IsKeyDown(aether::Key::Space);
			const float speed = reeling ? kReelSpeed : (kReelSpeed * 0.25f);

			if (distance > 0.01f)
			{
				m_bobberPos += glm::normalize(toRod) * speed * dt;
			}

			if (distance <= kCatchRadius)
			{
				m_score += 1;
				m_bobberState = BobberState::Ready;
				m_bobberPos = rodOrigin;
				if (m_hookedFishIndex >= 0 && m_hookedFishIndex < static_cast<int>(m_fishAgents.size()))
				{
					m_fishAgents[m_hookedFishIndex].hooked = false;
					std::uniform_real_distribution<float> angleDist(0.0f, glm::two_pi<float>());
					const float angle = angleDist(m_rng);
					std::uniform_real_distribution<float> radiusDist(4.0f, kFishWanderRadius);
					const float radius = radiusDist(m_rng);
					m_fishAgents[m_hookedFishIndex].pos = glm::vec3(std::cos(angle) * radius, kWaterLevel, std::sin(angle) * radius);
					m_fishAgents[m_hookedFishIndex].target = m_fishAgents[m_hookedFishIndex].pos + glm::vec3(2.0f, 0.0f, 0.0f);
				}
				m_hookedFishIndex = -1;
			}
		}

		// Update fish movement.
		for (std::size_t i = 0; i < m_fishAgents.size(); ++i)
		{
			FishAgent& agent = m_fishAgents[i];
			if (agent.hooked)
			{
				agent.pos = m_bobberPos;
			}
			else
			{
				const glm::vec3 delta = agent.target - agent.pos;
				const float dist = glm::length(delta);
				if (dist < 0.5f)
				{
					std::uniform_real_distribution<float> angleDist(0.0f, glm::two_pi<float>());
					const float angle = angleDist(m_rng);
					std::uniform_real_distribution<float> radiusDist(3.0f, kFishWanderRadius);
					const float radius = radiusDist(m_rng);
					agent.target = glm::vec3(std::cos(angle) * radius, kWaterLevel, std::sin(angle) * radius);
				}
				else
				{
					const glm::vec3 desired = delta / dist;
					const float desiredHeading = std::atan2(desired.x, desired.z);
					float diff = desiredHeading - agent.heading;
					while (diff > glm::pi<float>())
					{
						diff -= glm::two_pi<float>();
					}
					while (diff < -glm::pi<float>())
					{
						diff += glm::two_pi<float>();
					}
					agent.heading += std::clamp(diff, -kFishTurnSpeed * dt, kFishTurnSpeed * dt);
					agent.pos += glm::vec3(std::sin(agent.heading), 0.0f, std::cos(agent.heading)) * agent.speed * dt;
					if (glm::length(agent.pos) > kFishWanderRadius)
					{
						agent.pos = glm::normalize(glm::vec3{agent.pos.x, 0.0f, agent.pos.z}) * kFishWanderRadius;
					}
				}
			}
			agent.bobPhase += dt * kFishBobSpeed;
		}

		// Apply fish transforms.
		for (std::size_t i = 0; i < m_fishEntities.size(); ++i)
		{
			const aether::Entity entity = m_fishEntities[i];
			if (!entity.IsValid())
			{
				continue;
			}

			const FishAgent& agent = m_fishAgents[i];
			glm::mat4 xf = glm::translate(glm::mat4{1.0f}, agent.pos + glm::vec3{0.0f, std::sin(agent.bobPhase) * 0.08f, 0.0f});
			xf = glm::rotate(xf, agent.heading, glm::vec3{0.0f, 1.0f, 0.0f});
			xf = glm::scale(xf, glm::vec3{kFishScale});
			world.Get<aether::TransformComponent>(entity).localToWorld = xf;
		}

		// Apply bobber transform.
		if (m_bobberEntity.IsValid())
		{
			glm::mat4 xf = glm::translate(glm::mat4{1.0f}, m_bobberPos);
			xf = glm::scale(xf, glm::vec3{kBobberScale});
			world.Get<aether::TransformComponent>(m_bobberEntity).localToWorld = xf;
		}

		// Apply fishing line transform.
		if (m_lineEntity.IsValid())
		{
			const glm::vec3 rodOrigin = GetRodWorldOrigin();
			glm::mat4 lineXf;
			if (m_bobberState == BobberState::Ready)
			{
				lineXf = glm::scale(glm::translate(glm::mat4{1.0f}, rodOrigin), glm::vec3{0.0f});
			}
			else
			{
				lineXf = MakeLineTransform(rodOrigin, m_bobberPos, kLineThickness);
			}
			world.Get<aether::TransformComponent>(m_lineEntity).localToWorld = lineXf;
		}
	}

	void FishingGameSystem::OnUnregister(aether::World& world)
	{
		if (!m_services || !m_assets || !m_cameras)
		{
			return;
		}

		AE_INFO(aether::LogCategory::App, "FishingGameSystem unregistered.");

		std::vector<entt::entity> toDestroy;
		{
			auto allView = world.View<FishingEntityTag>();
			toDestroy.assign(allView.begin(), allView.end());
		}

		for (const entt::entity e: toDestroy)
		{
			world.Destroy(aether::Entity{static_cast<std::uint32_t>(entt::to_integral(e))});
		}

		m_fishAgents.clear();
		m_fishEntities.clear();

		m_assets->UnregisterMaterial(m_waterMaterial);
		m_assets->UnregisterMaterial(m_shoreMaterial);
		m_assets->UnregisterMaterial(m_fishMaterial);
		m_assets->UnregisterMaterial(m_bobberMaterial);
		m_assets->UnregisterMaterial(m_lineMaterial);

		if (m_playerCamera.IsValid())
		{
			m_cameras->Destroy(m_playerCamera);
			m_playerCamera = {};
		}

		m_planeMesh = nullptr;
		m_cubeMesh = nullptr;
		m_pipeline.Destroy();
	}
} // namespace aether::app
