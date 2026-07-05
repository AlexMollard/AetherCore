#include "systems/BehaviorSystem.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "assets/AssetManager.hpp"
#include "material/MaterialSystem.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	void BehaviorSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		auto& reg = world.GetRegistry();

		// Bob: sine Y around the base captured on first update (so a gizmo move
		// while paused re-bases naturally after the component is re-applied).
		for (auto&& [enttE, bob, tc]: reg.view<BobComponent, TransformComponent>().each())
		{
			if (!bob.baseCaptured)
			{
				bob.baseY = tc.localToWorld[3].y;
				bob.baseCaptured = true;
			}
			bob.time += dt;
			tc.localToWorld[3].y = bob.baseY + std::sin(bob.time * bob.frequency + bob.phase) * bob.amplitude;
		}

		// Spin: additive euler rate, translation/scale preserved.
		for (auto&& [enttE, spin, tc]: reg.view<SpinComponent, TransformComponent>().each())
		{
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(tc.localToWorld, pos, euler, scale);
			euler += spin.eulerDegPerSec * dt;
			tc.localToWorld = ComposeTransform(pos, euler, scale);
		}

		// Orbit: circular patrol around a center, facing along the tangent.
		// Children follow verbatim - same propagation rule as das set_transform
		// (the fox's mesh children ride along).
		for (auto&& [enttE, orbit, tc]: reg.view<OrbitComponent, TransformComponent>().each())
		{
			orbit.angleDeg = std::fmod(orbit.angleDeg + orbit.angularSpeedDeg * dt, 360.0f);
			const float rad = glm::radians(orbit.angleDeg);
			const glm::vec3 pos{orbit.center.x + std::cos(rad) * orbit.radius, orbit.height, orbit.center.z + std::sin(rad) * orbit.radius};

			glm::vec3 curPos{}, curEuler{}, curScale{};
			DecomposeTRS(tc.localToWorld, curPos, curEuler, curScale);
			// Tangent yaw for a +Y counter-clockwise orbit; yawOffset absorbs the
			// model's forward convention.
			const float yawDeg = -orbit.angleDeg + orbit.yawOffsetDeg;
			tc.localToWorld = ComposeTransform(pos, glm::vec3(0.0f, yawDeg, 0.0f), curScale);

			const Entity e = World::FromEntt(enttE);
			if (const auto* h = world.TryGet<HierarchyComponent>(e))
			{
				for (const Entity child: h->children)
				{
					if (auto* childTc = world.TryGet<TransformComponent>(child))
					{
						childTc->localToWorld = tc.localToWorld;
					}
				}
			}
		}

		// MaterialPulse: sine-lerped emissive through the instance setters -
		// identical values on multiple entities dedup to a single registry slot
		// per step (the Zone-M shared-material demo semantics, data-driven).
		auto& materials = m_assets.GetMaterialRegistry();
		auto& pipelines = m_assets.GetPipelineCache();
		for (auto&& [enttE, pulse]: reg.view<MaterialPulseComponent>().each())
		{
			pulse.time += dt;
			const float t = 0.5f + 0.5f * std::sin(pulse.time * pulse.frequency);
			const glm::vec3 emissive = glm::mix(pulse.emissiveA, pulse.emissiveB, t);
			MaterialSystem::SetEmissive(world, World::FromEntt(enttE), materials, pipelines, emissive);
		}
	}
} // namespace aether::app
