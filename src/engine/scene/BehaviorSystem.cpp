#include "scene/BehaviorSystem.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "assets/AssetManager.hpp"
#include "material/MaterialSystem.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/Components.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void BehaviorSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		auto& reg = world.GetRegistry();

		// All three transform behaviors move entities through
		// ecs::SetWorldTransform: the subtree rides along with the same
		// world-space delta, so children keep their relative offsets (the
		// fox's mesh children, or anything hand-parented in the editor).

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
			glm::mat4 m = tc.localToWorld;
			m[3].y = bob.baseY + std::sin(bob.time * bob.frequency + bob.phase) * bob.amplitude;
			ecs::SetWorldTransform(world, World::FromEntt(enttE), m);
		}

		// Spin: additive euler rate, translation/scale preserved.
		for (auto&& [enttE, spin, tc]: reg.view<SpinComponent, TransformComponent>().each())
		{
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(tc.localToWorld, pos, euler, scale);
			euler += spin.eulerDegPerSec * dt;
			ecs::SetWorldTransform(world, World::FromEntt(enttE), ComposeTransform(pos, euler, scale));
		}

		// Orbit: circular patrol around a center, facing along the tangent.
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
			ecs::SetWorldTransform(world, World::FromEntt(enttE), ComposeTransform(pos, glm::vec3(0.0f, yawDeg, 0.0f), curScale));
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
} // namespace aether
