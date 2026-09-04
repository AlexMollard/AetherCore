// Managed Physics2D surface. Runtime-safe: engine headers only, no editor
// catalogs, no ImGui. Mirrors PhysicsExports.cpp conventions (blittable ABI,
// per-call world/context lookups, silent no-ops on missing bodies).

#include "scripting/interop/InteropCommon.hpp"

#include <cstdint>
#include <vector>

#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/World.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

struct RaycastHit2D
{
	std::int32_t hit = 0;
	Vec2 point;
	Vec2 normal;
	float fraction = 1.0f;
	std::uint32_t entity = 0;
};

namespace
{
	aether::Physics2DSystem* System2D()
	{
		return ActiveContext().physics2D;
	}

	aether::Physics2DBodyHandle BodyOf(std::uint32_t id)
	{
		if (const auto* rigid = ActiveWorld().TryGet<aether::RigidBody2DComponent>(aether::Entity{id}))
		{
			return rigid->body;
		}
		return {};
	}

	aether::Body2DType BodyType(std::int32_t type)
	{
		switch (type)
		{
			case 0:
				return aether::Body2DType::Static;
			case 1:
				return aether::Body2DType::Kinematic;
			case 2:
			default:
				return aether::Body2DType::Dynamic;
		}
	}

	void AddBody(std::uint32_t id, const aether::Collider2DComponent& collider, std::int32_t type)
	{
		if (!EntityAlive(id))
		{
			return;
		}
		auto& world = ActiveWorld();
		const aether::Entity entity{id};
		world.EmplaceOrReplace<aether::Collider2DComponent>(entity, collider);
		world.EmplaceOrReplace<aether::RigidBody2DComponent>(entity, aether::RigidBody2DComponent{.bodyType = BodyType(type)});
	}

	RaycastHit2D FromHit(const aether::Physics2DSystem::RayHit2D& hit)
	{
		return {.hit = hit.hit ? 1 : 0, .point = {hit.point.x, hit.point.y}, .normal = {hit.normal.x, hit.normal.y}, .fraction = hit.fraction, .entity = hit.entity};
	}

	// Query results survive until the next overlap call on the SAME thread,
	// mirroring the 3D cache. thread_local so an RPC dispatched on a worker
	// thread cannot rebind the result set a game-thread script is indexing.
	thread_local std::vector<std::uint32_t> g_overlapCache;
} // namespace

// ── Bodies ────────────────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_physics2d_add_box(std::uint32_t id, Vec2 size, std::int32_t bodyType)
{ SafeExport([&] -> void { AddBody(id, aether::Collider2DComponent{.shape = aether::Collider2DShape::Box, .size = ToGlm(size)}, bodyType); }); }

AE_SCRIPT_API void aether_physics2d_add_circle(std::uint32_t id, float radius, std::int32_t bodyType)
{ SafeExport([&] -> void { AddBody(id, aether::Collider2DComponent{.shape = aether::Collider2DShape::Circle, .radius = radius}, bodyType); }); }

AE_SCRIPT_API void aether_physics2d_add_capsule(std::uint32_t id, float radius, float height, std::int32_t bodyType)
{ SafeExport([&] -> void { AddBody(id, aether::Collider2DComponent{.shape = aether::Collider2DShape::Capsule, .radius = radius, .capsuleHeight = height}, bodyType); }); }

AE_SCRIPT_API void aether_physics2d_set_trigger(std::uint32_t id, std::int32_t trigger)
{
	SafeExport([&] -> void
	{
	auto& world = ActiveWorld();
	const aether::Entity entity{id};
	if (auto* collider = world.TryGet<aether::Collider2DComponent>(entity))
	{
		collider->isTrigger = trigger != 0;
		if (auto* physics = System2D())
		{
			physics->RebuildBody(world, entity);
		}
	}
	});
}

// ── Velocity / forces ─────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_physics2d_set_linear_velocity(std::uint32_t id, Vec2 velocity)
{
	SafeExport([&] -> void
	{
	if (auto* physics = System2D())
	{
		physics->SetLinearVelocity(BodyOf(id), ToGlm(velocity));
	}
	});
}

AE_SCRIPT_API Vec2 aether_physics2d_get_linear_velocity(std::uint32_t id)
{
	return SafeExport([&] -> Vec2
	{
	if (const auto* physics = System2D())
	{
		const glm::vec2 v = physics->GetLinearVelocity(BodyOf(id));
		return {v.x, v.y};
	}
	return {};
	});
}

AE_SCRIPT_API void aether_physics2d_set_angular_velocity(std::uint32_t id, float radiansPerSec)
{
	SafeExport([&] -> void
	{
	if (auto* physics = System2D())
	{
		physics->SetAngularVelocity(BodyOf(id), radiansPerSec);
	}
	});
}

AE_SCRIPT_API float aether_physics2d_get_angular_velocity(std::uint32_t id)
{
	return SafeExport([&] -> float
	{
	const auto* physics = System2D();
	return physics != nullptr ? physics->GetAngularVelocity(BodyOf(id)) : 0.0f;
	});
}

AE_SCRIPT_API void aether_physics2d_add_force(std::uint32_t id, Vec2 force)
{
	SafeExport([&] -> void
	{
	if (auto* physics = System2D())
	{
		physics->ApplyForce(BodyOf(id), ToGlm(force));
	}
	});
}

AE_SCRIPT_API void aether_physics2d_add_impulse(std::uint32_t id, Vec2 impulse)
{
	SafeExport([&] -> void
	{
	if (auto* physics = System2D())
	{
		physics->ApplyLinearImpulse(BodyOf(id), ToGlm(impulse));
	}
	});
}

AE_SCRIPT_API void aether_physics2d_add_torque(std::uint32_t id, float torque)
{
	SafeExport([&] -> void
	{
	if (auto* physics = System2D())
	{
		physics->ApplyTorque(BodyOf(id), torque);
	}
	});
}

AE_SCRIPT_API void aether_physics2d_add_angular_impulse(std::uint32_t id, float impulse)
{
	SafeExport([&] -> void
	{
	if (auto* physics = System2D())
	{
		physics->ApplyAngularImpulse(BodyOf(id), impulse);
	}
	});
}

AE_SCRIPT_API void aether_physics2d_set_drop_through(std::uint32_t id, float seconds)
{
	SafeExport([&] -> void
	{
	if (auto* physics = System2D())
	{
		physics->SetDropThrough(ActiveWorld(), aether::Entity{id}, seconds);
	}
	});
}

AE_SCRIPT_API void aether_physics2d_set_gravity_scale(std::uint32_t id, float scale)
{
	SafeExport([&] -> void
	{
	if (auto* rigid = ActiveWorld().TryGet<aether::RigidBody2DComponent>(aether::Entity{id}))
	{
		rigid->gravityScale = scale;
	}
	if (auto* physics = System2D())
	{
		physics->SetGravityScale(BodyOf(id), scale);
	}
	});
}

// ── Queries ───────────────────────────────────────────────────────────────────

AE_SCRIPT_API RaycastHit2D aether_physics2d_raycast(Vec2 origin, Vec2 direction, float maxDistance)
{
	return SafeExport([&] -> RaycastHit2D
	{
	if (const auto* physics = System2D())
	{
		return FromHit(physics->CastRay(ToGlm(origin), ToGlm(direction), maxDistance));
	}
	return {};
	});
}

AE_SCRIPT_API RaycastHit2D aether_physics2d_circlecast(Vec2 origin, float radius, Vec2 direction, float maxDistance)
{
	return SafeExport([&] -> RaycastHit2D
	{
	if (const auto* physics = System2D())
	{
		return FromHit(physics->CastCircle(ToGlm(origin), radius, ToGlm(direction), maxDistance));
	}
	return {};
	});
}

// True if a solid (two-way) tile covers this world point - correct even deep inside a solid block,
// where the hollow chain colliders would report nothing.
AE_SCRIPT_API std::int32_t aether_physics2d_is_point_solid(Vec2 point)
{
	return SafeExport([&] -> std::int32_t
	{
	if (auto* physics = System2D())
	{
		return physics->IsWorldPointSolid(ActiveWorld(), ToGlm(point)) ? 1 : 0;
	}
	return 0;
	});
}

AE_SCRIPT_API std::int32_t aether_physics2d_overlap_circle(Vec2 center, float radius)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* physics = System2D();
	g_overlapCache = physics != nullptr ? physics->OverlapCircle(ToGlm(center), radius) : std::vector<std::uint32_t>{};
	return static_cast<std::int32_t>(g_overlapCache.size());
	});
}

AE_SCRIPT_API std::int32_t aether_physics2d_overlap_point(Vec2 point)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* physics = System2D();
	g_overlapCache = physics != nullptr ? physics->OverlapPoint(ToGlm(point)) : std::vector<std::uint32_t>{};
	return static_cast<std::int32_t>(g_overlapCache.size());
	});
}

AE_SCRIPT_API std::int32_t aether_physics2d_overlap_aabb(Vec2 min, Vec2 max)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* physics = System2D();
	g_overlapCache = physics != nullptr ? physics->OverlapAabb(ToGlm(min), ToGlm(max)) : std::vector<std::uint32_t>{};
	return static_cast<std::int32_t>(g_overlapCache.size());
	});
}

AE_SCRIPT_API std::uint32_t aether_physics2d_overlap_at(std::int32_t index)
{
	return SafeExport([&] -> std::uint32_t
	{
	if (index < 0 || static_cast<std::size_t>(index) >= g_overlapCache.size())
	{
		return 0;
	}
	return g_overlapCache[static_cast<std::size_t>(index)];
	});
}

// ── Collision / trigger events ────────────────────────────────────────────────

AE_SCRIPT_API void aether_physics2d_enable_events(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(id))
	{
		return;
	}
	auto& world = ActiveWorld();
	const aether::Entity entity{id};
	if (!world.Has<aether::CollisionEvents2DComponent>(entity))
	{
		world.GetRegistry().emplace<aether::CollisionEvents2DComponent>(aether::World::ToEntt(entity));
	}
	});
}

namespace
{
	const std::vector<aether::Entity>* EventList2D(std::uint32_t id, std::int32_t kind)
	{
		const auto* events = ActiveWorld().TryGet<aether::CollisionEvents2DComponent>(aether::Entity{id});
		if (events == nullptr)
		{
			return nullptr;
		}
		switch (kind)
		{
			case 0:
				return &events->collisionEnter;
			case 1:
				return &events->collisionExit;
			case 2:
				return &events->triggerEnter;
			case 3:
				return &events->triggerExit;
			case 4:
				return &events->overlapping;
			default:
				return nullptr;
		}
	}
} // namespace

AE_SCRIPT_API std::int32_t aether_physics2d_event_count(std::uint32_t id, std::int32_t kind)
{
	return SafeExport([&] -> std::int32_t
	{
	const std::vector<aether::Entity>* list = EventList2D(id, kind);
	return list != nullptr ? static_cast<std::int32_t>(list->size()) : 0;
	});
}

AE_SCRIPT_API std::uint32_t aether_physics2d_event_at(std::uint32_t id, std::int32_t kind, std::int32_t index)
{
	return SafeExport([&] -> std::uint32_t
	{
	const std::vector<aether::Entity>* list = EventList2D(id, kind);
	if (list == nullptr || index < 0 || static_cast<std::size_t>(index) >= list->size())
	{
		return 0;
	}
	return (*list)[static_cast<std::size_t>(index)].id;
	});
}
