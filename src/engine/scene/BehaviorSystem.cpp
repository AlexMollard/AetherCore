#include "scene/BehaviorSystem.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "assets/AssetManager.hpp"
#include "material/MaterialSystem.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
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

		for (auto&& [enttE, bob, tc]: reg.view<BobComponent, TransformComponent>().each())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttE)))
			{
				continue;
			}
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

		for (auto&& [enttE, spin, tc]: reg.view<SpinComponent, TransformComponent>().each())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttE)))
			{
				continue;
			}
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(tc.localToWorld, pos, euler, scale);
			euler += spin.eulerDegPerSec * dt;
			ecs::SetWorldTransform(world, World::FromEntt(enttE), ComposeTransform(pos, euler, scale));
		}

		for (auto&& [enttE, orbit, tc]: reg.view<OrbitComponent, TransformComponent>().each())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttE)))
			{
				continue;
			}
			orbit.angleDeg = std::fmod(orbit.angleDeg + orbit.angularSpeedDeg * dt, 360.0f);
			const float rad = glm::radians(orbit.angleDeg);
			const glm::vec3 pos{orbit.center.x + std::cos(rad) * orbit.radius, orbit.height, orbit.center.z + std::sin(rad) * orbit.radius};

			glm::vec3 curPos{}, curEuler{}, curScale{};
			DecomposeTRS(tc.localToWorld, curPos, curEuler, curScale);
			const float yawDeg = -orbit.angleDeg + orbit.yawOffsetDeg;
			ecs::SetWorldTransform(world, World::FromEntt(enttE), ComposeTransform(pos, glm::vec3(0.0f, yawDeg, 0.0f), curScale));
		}

		for (auto&& [enttE, pulse, tc]: reg.view<ScalePulseComponent, TransformComponent>().each())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttE)))
			{
				continue;
			}
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(tc.localToWorld, pos, euler, scale);
			if (!pulse.baseCaptured)
			{
				pulse.baseScale = scale;
				pulse.baseCaptured = true;
			}
			pulse.time += dt;
			const float factor = 1.0f + std::sin(pulse.time * pulse.frequency + pulse.phase) * pulse.amplitude;
			const glm::vec3 pulsed = glm::max(pulse.baseScale * factor, glm::vec3(0.001f));
			ecs::SetWorldTransform(world, World::FromEntt(enttE), ComposeTransform(pos, euler, pulsed));
		}

		for (auto&& [enttE, look, tc]: reg.view<LookAtComponent, TransformComponent>().each())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttE)))
			{
				continue;
			}
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(tc.localToWorld, pos, euler, scale);
			const glm::vec3 to = look.target - pos;
			if (glm::dot(to, to) < 1e-8f)
			{
				continue;
			}
			const glm::vec3 dir = glm::normalize(to);
			const glm::vec3 up = look.keepUpright && glm::abs(glm::dot(dir, glm::vec3(0, 1, 0))) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);
			glm::mat4 m = glm::mat4_cast(glm::quatLookAt(dir, up));
			m[0] *= scale.x;
			m[1] *= scale.y;
			m[2] *= scale.z;
			m[3] = glm::vec4(pos, 1.0f);
			ecs::SetWorldTransform(world, World::FromEntt(enttE), m);
		}

		// Parallax layers follow the main camera at a fraction of its motion.
		// Runs before the script system that drives the camera, so it samples
		// the same camera-entity transform the frame will render against.
		if (const Entity camEntity = ecs::GetMainCameraEntity(world); camEntity.IsValid())
		{
			glm::vec2 camPos{0.0f};
			if (const auto* camTc = world.TryGet<TransformComponent>(camEntity))
			{
				camPos = glm::vec2(camTc->localToWorld[3]);
			}
			for (auto&& [enttE, parallax, tc]: reg.view<ParallaxComponent, TransformComponent>().each())
			{
				if (ecs::HasDisabledAncestor(world, World::FromEntt(enttE)))
				{
					continue;
				}
				glm::vec3 pos{}, euler{}, scale{};
				DecomposeTRS(tc.localToWorld, pos, euler, scale);
				const glm::vec2 follow = glm::vec2(1.0f) - parallax.factor;
				if (!parallax.baseCaptured)
				{
					// Anchor so the first frame lands exactly on the authored
					// position (no snap when Play begins).
					parallax.base = glm::vec2(pos) - follow * camPos;
					parallax.baseCaptured = true;
				}
				parallax.time += dt;
				const glm::vec2 want = parallax.base + follow * camPos + parallax.scrollSpeed * parallax.time;
				pos.x = want.x;
				pos.y = want.y;
				ecs::SetWorldTransform(world, World::FromEntt(enttE), ComposeTransform(pos, euler, scale));
			}
		}

		auto& materials = m_assets.GetMaterialRegistry();
		auto& pipelines = m_assets.GetPipelineCache();
		for (auto&& [enttE, pulse]: reg.view<MaterialPulseComponent>().each())
		{
			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttE)))
			{
				continue;
			}
			pulse.time += dt;
			const float t = 0.5f + 0.5f * std::sin(pulse.time * pulse.frequency);
			const glm::vec3 emissive = glm::mix(pulse.emissiveA, pulse.emissiveB, t);
			MaterialSystem::SetEmissive(world, World::FromEntt(enttE), materials, pipelines, emissive);
		}
	}
} // namespace aether
