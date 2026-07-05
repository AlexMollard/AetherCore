#include "scripting/interop/InteropCommon.hpp"

#include <cstring>
#include <string>

#include <glm/gtc/quaternion.hpp>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

// Entity lifecycle + TransformComponent access exported to C#. Bodies match the
// old daScript WorldModule semantics (true-teleport of physics bodies, subtree
// delta propagation) so ported scripts behave identically.

using namespace aether::app::scripting::interop;

namespace
{
	// Carry a physics body with a script-driven transform change, exactly as the
	// editor's ApplyWorldTransform does — otherwise a body simulates away from the
	// visuals and the two owners fight over the transform each frame.
	void TeleportBodyToTransform(aether::World& w, aether::Entity e)
	{
		auto* ps = w.TryGet<aether::PhysicsStateComponent>(e);
		const auto* tc = w.TryGet<aether::TransformComponent>(e);
		if (ps == nullptr || tc == nullptr)
		{
			return;
		}

		glm::vec3 pos{}, euler{}, scale{};
		aether::DecomposeTRS(tc->localToWorld, pos, euler, scale);
		const glm::quat q = glm::angleAxis(glm::radians(euler.y), glm::vec3(0, 1, 0))
			* glm::angleAxis(glm::radians(euler.x), glm::vec3(1, 0, 0))
			* glm::angleAxis(glm::radians(euler.z), glm::vec3(0, 0, 1));
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
} // namespace

// ── Entity lifecycle ──────────────────────────────────────────────────────────

AE_SCRIPT_API std::uint32_t aether_entity_create()
{
	aether::World& w = ActiveWorld();
	const aether::Entity e = w.Create();
	w.Emplace<aether::NameComponent>(e, aether::NameComponent{.name = "Entity"});
	// Track as scene-owned so the layer tears it down on unload/reload.
	aether::app::scripting::ActiveContext().sceneEntities.push_back(e);
	return e.id;
}

AE_SCRIPT_API void aether_entity_destroy(std::uint32_t id)
{
	ActiveWorld().Destroy(aether::Entity{id});
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

// ── Name ──────────────────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_set_name(std::uint32_t id, const char* name)
{
	ActiveWorld().EmplaceOrReplace<aether::NameComponent>(
		aether::Entity{id}, aether::NameComponent{.name = name != nullptr ? name : ""});
}

// Writes the entity name into a caller-provided UTF-8 buffer, returning the byte
// length (excluding the null terminator), or 0 when unnamed. Buffer-fill avoids
// allocating a managed string owner across the boundary.
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

// ── TransformComponent ────────────────────────────────────────────────────────

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
