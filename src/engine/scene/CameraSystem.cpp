#include "scene/CameraSystem.hpp"

#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		glm::vec3 ForwardOf(const glm::mat4& localToWorld)
		{
			const glm::vec3 fwd = -glm::vec3(localToWorld[2]);
			const float len = glm::length(fwd);
			return len > 1e-6f ? fwd / len : glm::vec3(0.0f, 0.0f, -1.0f);
		}
	} // namespace

	void CameraSystem::Update(World& world, float)
	{
		AE_PROFILE_ZONE();
		auto& reg = world.GetRegistry();

		m_seenScratch.clear();
		m_mainBacking = {};

		for (auto&& [handle, orbit, tc]: reg.view<OrbitCameraComponent, TransformComponent>().each())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(handle)))
			{
				continue;
			}
			tc.localToWorld = ecs::OrbitCameraMatrix(orbit.target, orbit.yaw, orbit.pitch, orbit.distance);
		}

		for (const entt::entity handle: reg.view<CameraComponent, TransformComponent>())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(handle)))
			{
				continue;
			}

			auto& cam = reg.get<CameraComponent>(handle);
			const auto& tc = reg.get<TransformComponent>(handle);

			CameraHandle backing{cam.backingCamera};
			if (!backing.IsValid() || m_cameras.TryGet(backing) == nullptr)
			{
				CameraDesc desc;
				desc.mode = CameraMode::Manual;
				desc.fovDegrees = cam.fovDegrees;
				desc.nearPlane = cam.nearPlane;
				desc.farPlane = cam.farPlane;
				backing = m_cameras.Create(desc);
				cam.backingCamera = backing.id;
			}

			Camera* backingCam = m_cameras.TryGet(backing);
			if (backingCam == nullptr)
			{
				continue;
			}

			const glm::vec3 position = glm::vec3(tc.localToWorld[3]);
			const glm::vec3 forward = ForwardOf(tc.localToWorld);
			const float pitch = glm::degrees(std::asin(glm::clamp(forward.y, -1.0f, 1.0f)));
			const float yaw = glm::degrees(std::atan2(-forward.x, -forward.z));
			backingCam->SetMode(CameraMode::Manual);
			backingCam->SetPosition(position);
			backingCam->SetYawPitch(yaw, pitch);
			backingCam->SetPerspective(cam.fovDegrees, cam.nearPlane, cam.farPlane);

			m_seenScratch.insert(World::FromEntt(handle).id);
			m_backing[World::FromEntt(handle).id] = backing;

			if (reg.all_of<MainCameraComponent>(handle))
			{
				m_mainBacking = backing;
			}
		}

		std::vector<std::uint32_t> stale;
		for (const auto& [entityId, backing]: m_backing)
		{
			if (!m_seenScratch.contains(entityId))
			{
				stale.push_back(entityId);
			}
		}
		for (const std::uint32_t entityId: stale)
		{
			m_cameras.Destroy(m_backing[entityId]);
			m_backing.erase(entityId);
		}

		if (m_applyMainCamera && m_mainBacking.IsValid())
		{
			m_cameras.SetMainCamera(m_mainBacking);
		}
	}
} // namespace aether
