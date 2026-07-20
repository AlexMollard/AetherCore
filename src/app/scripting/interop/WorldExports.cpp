#include "scripting/interop/InteropCommon.hpp"

#include <cstring>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

using namespace aether::app::scripting::interop;

namespace
{
	void TeleportBodyToTransform(aether::World& w, aether::Entity e)
	{
		const auto* tc = w.TryGet<aether::TransformComponent>(e);
		if (tc == nullptr)
		{
			return;
		}

		if (auto* ps = w.TryGet<aether::PhysicsStateComponent>(e))
		{
			glm::vec3 pos{}, euler{}, scale{};
			aether::DecomposeTRS(tc->localToWorld, pos, euler, scale);
			const glm::quat q = glm::angleAxis(glm::radians(euler.y), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(euler.x), glm::vec3(1, 0, 0)) * glm::angleAxis(glm::radians(euler.z), glm::vec3(0, 0, 1));
			ps->prevPosition = pos;
			ps->currPosition = pos;
			ps->prevRotation = q;
			ps->currRotation = q;
			ps->scale = glm::max(scale, glm::vec3(0.001f));

			const auto* rb = w.TryGet<aether::RigidBodyComponent>(e);
			auto* physics = aether::app::scripting::ActiveContext().physics;
			if (rb != nullptr && physics != nullptr)
			{
				physics->SetPosition(rb->body, pos);
				physics->SetRotation(rb->body, q);
			}
		}

		// 2D bodies re-sync the transform from the body every frame, so a
		// script teleport must move the body too or it silently reverts.
		if (auto* physics2D = aether::app::scripting::ActiveContext().physics2D)
		{
			physics2D->TeleportToTransform(w, e);
		}
	}
} // namespace

AE_SCRIPT_API std::uint32_t aether_entity_create()
{
	aether::World& w = ActiveWorld();
	const aether::Entity e = w.Create();
	w.Emplace<aether::NameComponent>(e, aether::NameComponent{.name = "Entity"});
	aether::app::scripting::ActiveContext().sceneEntities.push_back(e);
	return e.id;
}

AE_SCRIPT_API void aether_entity_destroy(std::uint32_t id)
{
	// Deferred (Unity-style): scripts call this from inside their own
	// callbacks, where an immediate destroy would free the ScriptComponent
	// storage the runner is iterating. Flushed at the end of the script
	// update (ScriptComponentSystem); the entity stays valid until then.
	aether::app::scripting::ActiveContext().pendingDestroys.push_back(aether::Entity{id});
}

AE_SCRIPT_API std::int32_t aether_entity_valid(std::uint32_t id)
{
	return id != 0 ? 1 : 0;
}

AE_SCRIPT_API void aether_mark_transient(std::uint32_t id)
{
	auto& reg = ActiveWorld().GetRegistry();
	const auto e = aether::World::ToEntt(aether::Entity{id});
	if (reg.valid(e) && !reg.all_of<aether::SceneTransientComponent>(e))
	{
		reg.emplace<aether::SceneTransientComponent>(e);
	}
}

AE_SCRIPT_API void aether_set_name(std::uint32_t id, const char* name)
{
	ActiveWorld().EmplaceOrReplace<aether::NameComponent>(aether::Entity{id}, aether::NameComponent{.name = name != nullptr ? name : ""});
}

AE_SCRIPT_API std::int32_t aether_get_name(std::uint32_t id, char* buf, std::int32_t bufLen)
{
	const auto* nc = ActiveWorld().TryGet<aether::NameComponent>(aether::Entity{id});
	if (nc == nullptr || buf == nullptr || bufLen <= 0)
	{
		return 0;
	}
	const auto n = static_cast<std::int32_t>(nc->name.size());
	const std::int32_t copy = n < bufLen - 1 ? n : bufLen - 1;
	std::memcpy(buf, nc->name.data(), static_cast<size_t>(copy));
	buf[copy] = '\0';
	return copy;
}

AE_SCRIPT_API void aether_add_transform(std::uint32_t id)
{
	ActiveWorld().EmplaceOrReplace<aether::TransformComponent>(aether::Entity{id}, aether::TransformComponent{.localToWorld = glm::mat4(1.0f)});
}

AE_SCRIPT_API std::int32_t aether_has_transform(std::uint32_t id)
{
	return ActiveWorld().TryGet<aether::TransformComponent>(aether::Entity{id}) != nullptr ? 1 : 0;
}

AE_SCRIPT_API void aether_remove_transform(std::uint32_t id)
{
	ActiveWorld().Remove<aether::TransformComponent>(aether::Entity{id});
}

AE_SCRIPT_API Vec3 aether_get_position(std::uint32_t id)
{
	const auto* tc = ActiveWorld().TryGet<aether::TransformComponent>(aether::Entity{id});
	if (tc == nullptr)
	{
		return {};
	}
	return FromGlm(glm::vec3(tc->localToWorld[3]));
}

AE_SCRIPT_API void aether_set_position(std::uint32_t id, Vec3 pos)
{
	aether::World& w = ActiveWorld();
	const aether::Entity e{id};
	auto* tc = w.TryGet<aether::TransformComponent>(e);
	if (tc == nullptr)
	{
		return;
	}
	glm::mat4 m = tc->localToWorld;
	m[3] = glm::vec4(ToGlm(pos), 1.0f);
	aether::ecs::SetWorldTransform(w, e, m);
	TeleportBodyToTransform(w, e);
}

AE_SCRIPT_API Vec3 aether_get_euler(std::uint32_t id)
{
	const auto* tc = ActiveWorld().TryGet<aether::TransformComponent>(aether::Entity{id});
	if (tc == nullptr)
	{
		return {};
	}
	glm::vec3 pos{}, euler{}, scale{};
	aether::DecomposeTRS(tc->localToWorld, pos, euler, scale);
	return FromGlm(euler);
}

AE_SCRIPT_API void aether_set_euler(std::uint32_t id, Vec3 euler)
{
	aether::World& w = ActiveWorld();
	const aether::Entity e{id};
	auto* tc = w.TryGet<aether::TransformComponent>(e);
	if (tc == nullptr)
	{
		return;
	}
	glm::vec3 pos{}, curEuler{}, scale{};
	aether::DecomposeTRS(tc->localToWorld, pos, curEuler, scale);
	aether::ecs::SetWorldTransform(w, e, aether::ComposeTransform(pos, ToGlm(euler), scale));
	TeleportBodyToTransform(w, e);
}

AE_SCRIPT_API Vec3 aether_get_scale(std::uint32_t id)
{
	const auto* tc = ActiveWorld().TryGet<aether::TransformComponent>(aether::Entity{id});
	if (tc == nullptr)
	{
		return {1.0f, 1.0f, 1.0f};
	}
	const auto& m = tc->localToWorld;
	return FromGlm({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))});
}

AE_SCRIPT_API void aether_set_transform(std::uint32_t id, Vec3 pos, Vec3 euler, Vec3 scale)
{
	aether::World& w = ActiveWorld();
	const aether::Entity e{id};
	aether::ecs::SetWorldTransform(w, e, aether::ComposeTransform(ToGlm(pos), ToGlm(euler), ToGlm(scale)));
	TeleportBodyToTransform(w, e);
}

AE_SCRIPT_API void aether_add_bob(std::uint32_t id, float amplitude, float frequency, float phase)
{
	ActiveWorld().EmplaceOrReplace<aether::BobComponent>(aether::Entity{id}, aether::BobComponent{.amplitude = amplitude, .frequency = frequency, .phase = phase});
}

AE_SCRIPT_API void aether_add_spin(std::uint32_t id, Vec3 eulerDegPerSec)
{
	ActiveWorld().EmplaceOrReplace<aether::SpinComponent>(aether::Entity{id}, aether::SpinComponent{.eulerDegPerSec = ToGlm(eulerDegPerSec)});
}

AE_SCRIPT_API void aether_add_orbit(std::uint32_t id, Vec3 center, float radius, float speedDeg, float startAngleDeg, float yawOffsetDeg, float height)
{
	ActiveWorld().EmplaceOrReplace<aether::OrbitComponent>(aether::Entity{id}, aether::OrbitComponent{.center = ToGlm(center), .radius = radius, .angularSpeedDeg = speedDeg, .angleDeg = startAngleDeg, .yawOffsetDeg = yawOffsetDeg, .height = height});
}

AE_SCRIPT_API void aether_add_material_pulse(std::uint32_t id, Vec3 emissiveA, Vec3 emissiveB, float frequency)
{
	ActiveWorld().EmplaceOrReplace<aether::MaterialPulseComponent>(aether::Entity{id}, aether::MaterialPulseComponent{.emissiveA = ToGlm(emissiveA), .emissiveB = ToGlm(emissiveB), .frequency = frequency});
}

AE_SCRIPT_API void aether_add_script(std::uint32_t id, const char* typeName)
{
	auto& world = ActiveWorld();
	const aether::Entity entity{id};
	auto* scripts = world.TryGet<aether::ScriptComponent>(entity);
	if (scripts == nullptr)
	{
		scripts = &world.Emplace<aether::ScriptComponent>(entity);
	}
	scripts->scripts.push_back(aether::ScriptEntry{.path = typeName != nullptr ? typeName : ""});
}

AE_SCRIPT_API std::int32_t aether_scene_file_exists(const char* name)
{
	return aether::app::scene::ReadSceneFile(name != nullptr ? name : "").has_value() ? 1 : 0;
}

AE_SCRIPT_API std::uint32_t aether_tag_create(const char* name)
{
	return aether::TagCreate(name != nullptr ? name : "");
}

AE_SCRIPT_API std::uint32_t aether_tag_get_id(const char* name)
{
	return aether::TagGetId(name != nullptr ? name : "");
}

AE_SCRIPT_API void aether_tag_add(std::uint32_t entityId, std::uint32_t tagId)
{
	aether::TagAdd(&ActiveWorld(), entityId, tagId);
}

AE_SCRIPT_API std::int32_t aether_tag_has(std::uint32_t entityId, std::uint32_t tagId)
{
	return aether::TagHas(&ActiveWorld(), entityId, tagId) ? 1 : 0;
}

AE_SCRIPT_API void aether_tag_remove(std::uint32_t entityId, std::uint32_t tagId)
{
	aether::TagRemove(&ActiveWorld(), entityId, tagId);
}

AE_SCRIPT_API std::int32_t aether_world_get_entities_with_transform(std::uint32_t* buf, std::int32_t cap)
{
	if (buf == nullptr || cap <= 0)
	{
		return 0;
	}
	std::int32_t n = 0;
	for (const auto enttE: ActiveWorld().View<aether::TransformComponent>())
	{
		if (n >= cap)
		{
			break;
		}
		buf[n++] = aether::World::FromEntt(enttE).id;
	}
	return n;
}

AE_SCRIPT_API std::int32_t aether_tag_get_entities(std::uint32_t tagId, std::uint32_t* buf, std::int32_t cap)
{
	if (buf == nullptr || cap <= 0)
	{
		return 0;
	}
	std::int32_t n = 0;
	aether::ForEachWithTag(&ActiveWorld(),
	        tagId,
	        [&](std::uint32_t id)
	        {
		        if (n < cap)
		        {
			        buf[n++] = id;
		        }
	        });
	return n;
}
