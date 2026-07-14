#include "scripting/interop/InteropCommon.hpp"

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/World.hpp"

#include <cstdint>
#include <vector>

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

struct RaycastHit
{
	std::int32_t hit = 0;
	Vec3 position;
	Vec3 normal;
	float fraction = 1.0f;
	std::uint32_t entity = 0;
};

namespace
{
	aether::PhysicsBodyHandle BodyOf(std::uint32_t id)
	{
		if (const auto* rb = ActiveWorld().TryGet<aether::RigidBodyComponent>(aether::Entity{id}))
		{
			return rb->body;
		}
		return {};
	}
} // namespace

namespace
{
	aether::PhysicsMotionType MotionType(std::int32_t dynamic)
	{
		return dynamic != 0 ? aether::PhysicsMotionType::Dynamic : aether::PhysicsMotionType::Static;
	}
} // namespace

namespace
{
	void AddCollider(std::uint32_t id, const aether::ColliderComponent& collider, std::int32_t dynamic)
	{
		auto& world = ActiveWorld();
		const aether::Entity entity{id};
		world.EmplaceOrReplace<aether::ColliderComponent>(entity, collider);
		world.EmplaceOrReplace<aether::RigidBodyComponent>(entity, aether::RigidBodyComponent{.motionType = MotionType(dynamic)});
	}
} // namespace

AE_SCRIPT_API void aether_physics_add_box(std::uint32_t id, Vec3 halfExtents, std::int32_t dynamic)
{
	AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Box, .halfExtents = ToGlm(halfExtents)}, dynamic);
}

AE_SCRIPT_API void aether_physics_add_sphere(std::uint32_t id, float radius, std::int32_t dynamic)
{
	AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Sphere, .radius = radius}, dynamic);
}

AE_SCRIPT_API void aether_physics_add_capsule(std::uint32_t id, float halfHeight, float radius, std::int32_t dynamic)
{
	AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Capsule, .radius = radius, .halfHeight = halfHeight}, dynamic);
}

AE_SCRIPT_API void aether_physics_set_linear_velocity(std::uint32_t id, Vec3 velocity)
{
	auto* phys = ActiveContext().physics;
	if (phys == nullptr)
	{
		return;
	}
	if (const auto* rb = ActiveWorld().TryGet<aether::RigidBodyComponent>(aether::Entity{id}))
	{
		phys->SetLinearVelocity(rb->body, ToGlm(velocity));
	}
}

AE_SCRIPT_API Vec3 aether_physics_get_linear_velocity(std::uint32_t id)
{
	if (auto* phys = ActiveContext().physics)
	{
		if (const auto* rb = ActiveWorld().TryGet<aether::RigidBodyComponent>(aether::Entity{id}))
		{
			return FromGlm(phys->GetLinearVelocity(rb->body));
		}
	}
	return {};
}

AE_SCRIPT_API Vec3 aether_physics_get_position(std::uint32_t id)
{
	const auto* ps = ActiveWorld().TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
	return ps != nullptr ? FromGlm(ps->currPosition) : Vec3{};
}

AE_SCRIPT_API Vec3 aether_physics_get_scale(std::uint32_t id)
{
	const auto* ps = ActiveWorld().TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
	return ps != nullptr ? FromGlm(ps->scale) : Vec3{1.0f, 1.0f, 1.0f};
}

AE_SCRIPT_API void aether_physics_set_debug_enabled(std::int32_t enabled)
{
	aether::SetPhysicsDebugShapesEnabled(enabled != 0);
}

AE_SCRIPT_API std::int32_t aether_physics_is_debug_enabled()
{
	return aether::IsPhysicsDebugShapesEnabled() ? 1 : 0;
}

AE_SCRIPT_API void aether_physics_set_angular_velocity(std::uint32_t id, Vec3 velocity)
{
	if (auto* phys = ActiveContext().physics)
	{
		phys->SetAngularVelocity(BodyOf(id), ToGlm(velocity));
	}
}

AE_SCRIPT_API Vec3 aether_physics_get_angular_velocity(std::uint32_t id)
{
	if (auto* phys = ActiveContext().physics)
	{
		return FromGlm(phys->GetAngularVelocity(BodyOf(id)));
	}
	return {};
}

AE_SCRIPT_API void aether_physics_add_force(std::uint32_t id, Vec3 force)
{
	if (auto* phys = ActiveContext().physics)
	{
		phys->AddForce(BodyOf(id), ToGlm(force));
	}
}

AE_SCRIPT_API void aether_physics_add_impulse(std::uint32_t id, Vec3 impulse)
{
	if (auto* phys = ActiveContext().physics)
	{
		phys->AddImpulse(BodyOf(id), ToGlm(impulse));
	}
}

AE_SCRIPT_API void aether_physics_add_torque(std::uint32_t id, Vec3 torque)
{
	if (auto* phys = ActiveContext().physics)
	{
		phys->AddTorque(BodyOf(id), ToGlm(torque));
	}
}

AE_SCRIPT_API void aether_physics_add_angular_impulse(std::uint32_t id, Vec3 impulse)
{
	if (auto* phys = ActiveContext().physics)
	{
		phys->AddAngularImpulse(BodyOf(id), ToGlm(impulse));
	}
}

AE_SCRIPT_API void aether_physics_freeze_rotation(std::uint32_t id, std::int32_t x, std::int32_t y, std::int32_t z)
{
	auto* phys = ActiveContext().physics;
	auto* rb = ActiveWorld().TryGet<aether::RigidBodyComponent>(aether::Entity{id});
	if (phys == nullptr || rb == nullptr)
	{
		return;
	}
	rb->lockRotation = glm::bvec3(x != 0, y != 0, z != 0);
	phys->RebuildBody(ActiveWorld(), aether::Entity{id});
}

AE_SCRIPT_API RaycastHit aether_physics_raycast(Vec3 origin, Vec3 direction, float maxDistance)
{
	RaycastHit out;
	if (auto* phys = ActiveContext().physics)
	{
		const aether::PhysicsSystem::RaycastResult r = phys->CastRay(ToGlm(origin), ToGlm(direction), maxDistance);
		out.hit = r.hit ? 1 : 0;
		out.position = FromGlm(r.position);
		out.normal = FromGlm(r.normal);
		out.fraction = r.fraction;
		out.entity = r.entity;
	}
	return out;
}

AE_SCRIPT_API RaycastHit aether_physics_spherecast(Vec3 origin, Vec3 direction, float radius, float maxDistance)
{
	RaycastHit out;
	if (auto* phys = ActiveContext().physics)
	{
		const aether::PhysicsSystem::RaycastResult r = phys->SphereCast(ToGlm(origin), ToGlm(direction), radius, maxDistance);
		out.hit = r.hit ? 1 : 0;
		out.position = FromGlm(r.position);
		out.normal = FromGlm(r.normal);
		out.fraction = r.fraction;
		out.entity = r.entity;
	}
	return out;
}

namespace
{
	std::vector<std::uint32_t> g_overlapCache;
}

AE_SCRIPT_API std::int32_t aether_physics_overlap_sphere(Vec3 center, float radius)
{
	g_overlapCache.clear();
	if (auto* phys = ActiveContext().physics)
	{
		g_overlapCache = phys->OverlapSphere(ToGlm(center), radius);
	}
	return static_cast<std::int32_t>(g_overlapCache.size());
}

AE_SCRIPT_API std::uint32_t aether_physics_overlap_at(std::int32_t index)
{
	if (index < 0 || static_cast<std::size_t>(index) >= g_overlapCache.size())
	{
		return 0;
	}
	return g_overlapCache[static_cast<std::size_t>(index)];
}

AE_SCRIPT_API void aether_physics_enable_events(std::uint32_t id)
{
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (!world.Has<aether::CollisionEventsComponent>(e))
	{
		world.GetRegistry().emplace<aether::CollisionEventsComponent>(aether::World::ToEntt(e));
	}
}

namespace
{
	const std::vector<aether::Entity>* EventList(std::uint32_t id, std::int32_t kind)
	{
		const auto* ev = ActiveWorld().TryGet<aether::CollisionEventsComponent>(aether::Entity{id});
		if (ev == nullptr)
		{
			return nullptr;
		}
		switch (kind)
		{
			case 0:
				return &ev->collisionEnter;
			case 1:
				return &ev->collisionExit;
			case 2:
				return &ev->triggerEnter;
			case 3:
				return &ev->triggerExit;
			case 4:
				return &ev->overlapping;
			default:
				return nullptr;
		}
	}
} // namespace

AE_SCRIPT_API std::int32_t aether_physics_event_count(std::uint32_t id, std::int32_t kind)
{
	const std::vector<aether::Entity>* list = EventList(id, kind);
	return list != nullptr ? static_cast<std::int32_t>(list->size()) : 0;
}

AE_SCRIPT_API std::uint32_t aether_physics_event_at(std::uint32_t id, std::int32_t kind, std::int32_t index)
{
	const std::vector<aether::Entity>* list = EventList(id, kind);
	if (list == nullptr || index < 0 || static_cast<std::size_t>(index) >= list->size())
	{
		return 0;
	}
	return (*list)[static_cast<std::size_t>(index)].id;
}
