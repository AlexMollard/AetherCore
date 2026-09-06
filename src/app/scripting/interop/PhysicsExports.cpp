#include "scripting/interop/InteropCommon.hpp"

#include "net/NetworkContext.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

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
	aether::RigidBodyComponent* RigidBodyOf(std::uint32_t id)
	{
		return ActiveWorld().TryGet<aether::RigidBodyComponent>(aether::Entity{id});
	}

	aether::net::NetworkContext* Context()
	{
		const auto& ctx = ActiveContext();
		if (ctx.services == nullptr)
		{
			return nullptr;
		}
		return ctx.services->TryGet<aether::net::NetworkContext>();
	}

	// Same reasoning and shape as CharacterExports.cpp's CanControl: velocity/force/
	// impulse/activation writes on an existing body must not be drivable by a peer
	// that does not own the entity - SyncSimulationAuthority already forces a
	// non-owned RigidBody Kinematic, but that stops the SOLVER from acting on it, not
	// a script from writing to it, and this is the export layer's own guard rather
	// than trusting every caller (a grab controller, an RPC handler, ...) to check
	// ownership itself first. Verified against the physics gun's own script: it
	// splits claim (Net.RequestOwnership) from drive (Physics.SetLinearVelocity/
	// AddImpulseAtPoint) and only starts driving once Net.IsOwner is confirmed - so
	// this never rejects a write the existing grab flow makes, only a write nobody's
	// authority flow made in the first place.
	bool CanControl(std::uint32_t id)
	{
		const aether::net::NetworkContext* context = Context();
		return context == nullptr || context->HasAuthority(ActiveWorld(), aether::Entity{id});
	}

	// A no-op that must not pass silently: `what` landed on an entity whose Jolt body has
	// not been created yet (queued this frame by AddBoxBody/AddSphereBody/AddCapsuleBody,
	// baked next frame by PhysicsSystem::FlushPendingBodies) and had nothing to act on -
	// unlike a velocity, a force/torque/impulse has no persisted "initial" state to seed.
	// Warns exactly once per RigidBodyComponent instance (see
	// RigidBodyComponent::deferredCallWarned) so a script calling this every OnUpdate
	// before the body bakes warns once, not every frame; a warning that keeps recurring
	// means the body never baked at all (see FlushPendingBodies' own warning for why).
	void WarnDeferredNoOp(aether::RigidBodyComponent& rb, std::uint32_t id, const char* what)
	{
		if (rb.deferredCallWarned)
		{
			return;
		}
		rb.deferredCallWarned = true;
		AE_WARN(aether::LogCategory::Engine,
		        "Physics.{} on entity {} had no effect: its body has not been created yet "
		        "(queued this frame, baked next). If this keeps recurring for the same "
		        "entity, the body failed to create - look for FlushPendingBodies' own "
		        "warning naming it.",
		        what, id);
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
		if (!EntityAlive(id))
		{
			return;
		}
		auto& world = ActiveWorld();
		const aether::Entity entity{id};
		world.EmplaceOrReplace<aether::ColliderComponent>(entity, collider);
		world.EmplaceOrReplace<aether::RigidBodyComponent>(entity, aether::RigidBodyComponent{.motionType = MotionType(dynamic)});
	}
} // namespace

AE_SCRIPT_API void aether_physics_add_box(std::uint32_t id, Vec3 halfExtents, std::int32_t dynamic)
{ SafeExport([&] -> void { AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Box, .halfExtents = ToGlm(halfExtents)}, dynamic); }); }

AE_SCRIPT_API void aether_physics_add_sphere(std::uint32_t id, float radius, std::int32_t dynamic)
{ SafeExport([&] -> void { AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Sphere, .radius = radius}, dynamic); }); }

AE_SCRIPT_API void aether_physics_add_capsule(std::uint32_t id, float halfHeight, float radius, std::int32_t dynamic)
{ SafeExport([&] -> void { AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Capsule, .radius = radius, .halfHeight = halfHeight}, dynamic); }); }

AE_SCRIPT_API void aether_physics_add_cylinder(std::uint32_t id, float halfHeight, float radius, std::int32_t dynamic)
{ SafeExport([&] -> void { AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Cylinder, .radius = radius, .halfHeight = halfHeight}, dynamic); }); }

AE_SCRIPT_API void aether_physics_add_convex_hull(std::uint32_t id, const char* meshSourceC, std::int32_t dynamic)
{ SafeExport([&] -> void { AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::ConvexHull, .meshSource = meshSourceC != nullptr ? meshSourceC : ""}, dynamic); }); }

// Static only - PhysicsSystem::FlushPendingBodies rejects (with a warning) a Mesh
// collider on anything else, since Jolt's own MeshShape::MustBeStatic() is advisory
// and not self-enforced. Still takes `dynamic` so a caller who gets this wrong sees
// that warning instead of a silently different signature to remember.
AE_SCRIPT_API void aether_physics_add_mesh(std::uint32_t id, const char* meshSourceC, std::int32_t dynamic)
{ SafeExport([&] -> void { AddCollider(id, aether::ColliderComponent{.shape = aether::PhysicsShapeType::Mesh, .meshSource = meshSourceC != nullptr ? meshSourceC : ""}, dynamic); }); }

// The one script path to a Kinematic body: every Add*Box/Sphere/Capsule/Cylinder/
// ConvexHull/Mesh body above only ever takes Static or Dynamic (see the MotionType()
// helper just above AddCollider). PhysicsSystem::SetBodyMotionType already existed and
// already handles Kinematic in place (Jolt's own BodyInterface::SetMotionType, no
// destroy/rebuild) - NetworkContext::SyncSimulationAuthority was its only caller. This
// just surfaces the same method to script. Gated by CanControl for the same reason
// SetLinearVelocity/AddForce/... are just below: a motion-type flip on a body this
// caller does not own is exactly the write that gate exists to stop - letting a
// non-owner silently freeze/unfreeze a peer's own body out from under that peer's
// authority would be a networking bug, not a feature.
AE_SCRIPT_API void aether_physics_set_motion_type(std::uint32_t id, std::int32_t motionType)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	if (phys == nullptr || !EntityAlive(id) || !CanControl(id))
	{
		return;
	}
	phys->SetBodyMotionType(ActiveWorld(), aether::Entity{id}, static_cast<aether::PhysicsMotionType>(motionType));
	});
}

AE_SCRIPT_API void aether_physics_set_linear_velocity(std::uint32_t id, Vec3 velocity)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->SetLinearVelocity(rb->body, ToGlm(velocity));
	}
	else
	{
		// Body not baked yet (queued this frame by AddBoxBody/... , created next frame
		// by FlushPendingBodies) - seed the value creation itself applies, so "add a
		// body, then set its velocity" works the same whether or not the body exists.
		rb->initialVelocity = ToGlm(velocity);
	}
	});
}

AE_SCRIPT_API Vec3 aether_physics_get_linear_velocity(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	auto* phys = ActiveContext().physics;
	const auto* rb = RigidBodyOf(id);
	if (rb == nullptr)
	{
		return {};
	}
	if (rb->body.IsValid() && phys != nullptr)
	{
		return FromGlm(phys->GetLinearVelocity(rb->body));
	}
	// Mirrors the setter: before the body exists, its velocity IS whatever was seeded,
	// so a script that sets then immediately reads back sees its own write.
	return FromGlm(rb->initialVelocity);
	});
}

AE_SCRIPT_API Vec3 aether_physics_get_position(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	const auto* ps = ActiveWorld().TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
	return ps != nullptr ? FromGlm(ps->currPosition) : Vec3{};
	});
}

AE_SCRIPT_API Vec3 aether_physics_get_scale(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	const auto* ps = ActiveWorld().TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
	return ps != nullptr ? FromGlm(ps->scale) : Vec3{1.0f, 1.0f, 1.0f};
	});
}

AE_SCRIPT_API void aether_physics_set_debug_enabled(std::int32_t enabled)
{ SafeExport([&] -> void { aether::SetPhysicsDebugShapesEnabled(enabled != 0); }); }

AE_SCRIPT_API std::int32_t aether_physics_is_debug_enabled()
{ return SafeExport([&] -> std::int32_t { return aether::IsPhysicsDebugShapesEnabled() ? 1 : 0; }); }

AE_SCRIPT_API void aether_physics_set_angular_velocity(std::uint32_t id, Vec3 velocity)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->SetAngularVelocity(rb->body, ToGlm(velocity));
	}
	else
	{
		rb->initialAngularVelocity = ToGlm(velocity);
	}
	});
}

AE_SCRIPT_API Vec3 aether_physics_get_angular_velocity(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	auto* phys = ActiveContext().physics;
	const auto* rb = RigidBodyOf(id);
	if (rb == nullptr)
	{
		return {};
	}
	if (rb->body.IsValid() && phys != nullptr)
	{
		return FromGlm(phys->GetAngularVelocity(rb->body));
	}
	return FromGlm(rb->initialAngularVelocity);
	});
}

AE_SCRIPT_API void aether_physics_add_force(std::uint32_t id, Vec3 force)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->AddForce(rb->body, ToGlm(force));
	}
	else
	{
		// A continuous per-step force has no persisted "initial" state to seed - Jolt
		// itself only ever applies AddForce for the step it was called in, so there is
		// nothing here for FlushPendingBodies to apply later even in principle.
		WarnDeferredNoOp(*rb, id, "AddForce");
	}
	});
}

AE_SCRIPT_API void aether_physics_add_impulse(std::uint32_t id, Vec3 impulse)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->AddImpulse(rb->body, ToGlm(impulse));
	}
	else
	{
		// An impulse divides by mass, which is not resolved until the body is created
		// (shape + density decide it unless RigidBodyComponent.mass overrides) - there is
		// no correct velocity to seed without duplicating that computation, so this is
		// genuinely lost rather than deferred. Say so instead of silently discarding it.
		WarnDeferredNoOp(*rb, id, "AddImpulse");
	}
	});
}

AE_SCRIPT_API void aether_physics_add_torque(std::uint32_t id, Vec3 torque)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->AddTorque(rb->body, ToGlm(torque));
	}
	else
	{
		WarnDeferredNoOp(*rb, id, "AddTorque");
	}
	});
}

AE_SCRIPT_API void aether_physics_add_angular_impulse(std::uint32_t id, Vec3 impulse)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->AddAngularImpulse(rb->body, ToGlm(impulse));
	}
	else
	{
		// Depends on the body's inertia tensor, resolved at the same point as mass -
		// same reasoning as AddImpulse above.
		WarnDeferredNoOp(*rb, id, "AddAngularImpulse");
	}
	});
}

// Applied at a WORLD POINT rather than the centre of mass, so it imparts spin - what makes
// a thrown prop tumble instead of sliding flat. The physics gun's primary "throw" primitive.
AE_SCRIPT_API void aether_physics_add_impulse_at_point(std::uint32_t id, Vec3 impulse, Vec3 point)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->AddImpulseAtPoint(rb->body, ToGlm(impulse), ToGlm(point));
	}
	else
	{
		// Same reasoning as AddImpulse: depends on mass (and here also on the point's
		// offset from the centre of mass, via the inertia tensor), neither resolved until
		// the body exists.
		WarnDeferredNoOp(*rb, id, "AddImpulseAtPoint");
	}
	});
}

// A grab controller's other primitives: zero a held prop's gravity, wake a sleeping one
// the instant it is picked up, and dial in per-pickup friction/restitution. Unlike the
// impulses above, all three have an authored field FlushPendingBodies already applies at
// creation (ColliderComponent::friction/restitution, RigidBodyComponent::gravityFactor/
// startActive) - so a call landing before the body exists seeds that field instead of
// warning: "add a body, then configure it" works the same whether or not it has been
// baked yet, exactly like Physics.SetLinearVelocity.
AE_SCRIPT_API void aether_physics_set_gravity_factor(std::uint32_t id, float factor)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->SetGravityFactor(rb->body, factor);
	}
	else
	{
		rb->gravityFactor = factor;
	}
	});
}

AE_SCRIPT_API void aether_physics_set_friction(std::uint32_t id, float friction)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->SetFriction(rb->body, friction);
	}
	else if (auto* collider = ActiveWorld().TryGet<aether::ColliderComponent>(aether::Entity{id}))
	{
		collider->friction = friction;
	}
	});
}

AE_SCRIPT_API void aether_physics_set_restitution(std::uint32_t id, float restitution)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->SetRestitution(rb->body, restitution);
	}
	else if (auto* collider = ActiveWorld().TryGet<aether::ColliderComponent>(aether::Entity{id}))
	{
		collider->restitution = restitution;
	}
	});
}

AE_SCRIPT_API void aether_physics_set_body_active(std::uint32_t id, std::int32_t active)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = RigidBodyOf(id);
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	if (rb->body.IsValid())
	{
		phys->SetBodyActive(rb->body, active != 0);
	}
	else
	{
		// Seeds the field FlushPendingBodies reads to decide whether to activate the
		// body the moment it is created (bi.AddBody(..., startActive ? Activate : ...)).
		rb->startActive = active != 0;
	}
	});
}

AE_SCRIPT_API std::int32_t aether_physics_is_body_active(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	auto* phys = ActiveContext().physics;
	const auto* rb = RigidBodyOf(id);
	if (rb == nullptr)
	{
		return 0;
	}
	if (rb->body.IsValid() && phys != nullptr)
	{
		return phys->IsBodyActive(rb->body) ? 1 : 0;
	}
	return rb->startActive ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_physics_freeze_rotation(std::uint32_t id, std::int32_t x, std::int32_t y, std::int32_t z)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	auto* rb = ActiveWorld().TryGet<aether::RigidBodyComponent>(aether::Entity{id});
	if (phys == nullptr || rb == nullptr || !CanControl(id))
	{
		return;
	}
	rb->lockRotation = glm::bvec3(x != 0, y != 0, z != 0);
	phys->RebuildBody(ActiveWorld(), aether::Entity{id});
	});
}

AE_SCRIPT_API RaycastHit aether_physics_raycast(Vec3 origin, Vec3 direction, float maxDistance)
{
	return SafeExport([&] -> RaycastHit
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
	});
}

AE_SCRIPT_API RaycastHit aether_physics_spherecast(Vec3 origin, Vec3 direction, float radius, float maxDistance)
{
	return SafeExport([&] -> RaycastHit
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
	});
}

namespace
{
	// Per-thread: an inbound RPC dispatched on a worker thread (or any second
	// caller reaching the export between Count() and At()) must not rebind the
	// result set another script is still indexing.
	thread_local std::vector<std::uint32_t> g_overlapCache;
}

AE_SCRIPT_API std::int32_t aether_physics_overlap_sphere(Vec3 center, float radius)
{
	return SafeExport([&] -> std::int32_t
	{
	g_overlapCache.clear();
	if (auto* phys = ActiveContext().physics)
	{
		g_overlapCache = phys->OverlapSphere(ToGlm(center), radius);
	}
	return static_cast<std::int32_t>(g_overlapCache.size());
	});
}

AE_SCRIPT_API std::uint32_t aether_physics_overlap_at(std::int32_t index)
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

AE_SCRIPT_API void aether_physics_enable_events(std::uint32_t id)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(id))
	{
		return;
	}
	auto& world = ActiveWorld();
	const aether::Entity e{id};
	if (!world.Has<aether::CollisionEventsComponent>(e))
	{
		world.GetRegistry().emplace<aether::CollisionEventsComponent>(aether::World::ToEntt(e));
	}
	});
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
	return SafeExport([&] -> std::int32_t
	{
	const std::vector<aether::Entity>* list = EventList(id, kind);
	return list != nullptr ? static_cast<std::int32_t>(list->size()) : 0;
	});
}

AE_SCRIPT_API std::uint32_t aether_physics_event_at(std::uint32_t id, std::int32_t kind, std::int32_t index)
{
	return SafeExport([&] -> std::uint32_t
	{
	const std::vector<aether::Entity>* list = EventList(id, kind);
	if (list == nullptr || index < 0 || static_cast<std::size_t>(index) >= list->size())
	{
		return 0;
	}
	return (*list)[static_cast<std::size_t>(index)].id;
	});
}

// -- Constraints (weld/rope tool-gun modes) -------------------------------------
// Both take the "self" entity as the constraint's OWN owner (matches JointComponent's
// convention: the entity holding the joint is body A, target is body B) and gate on
// CanControl the same way every other body-mutating export above does - welding a
// peer's prop out from under their authority is the identical class of bug as
// driving their velocity would be. targetId == 0 pins to the world, exactly like
// JointComponent leaving `target` at its default Entity{}.
AE_SCRIPT_API std::uint32_t aether_physics_create_weld(std::uint32_t id, std::uint32_t targetId)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto* phys = ActiveContext().physics;
	if (phys == nullptr || !EntityAlive(id) || !CanControl(id))
	{
		return 0;
	}
	if (targetId != 0 && !EntityAlive(targetId))
	{
		return 0;
	}
	return phys->CreateFixedConstraint(ActiveWorld(), aether::Entity{id}, aether::Entity{targetId});
	});
}

AE_SCRIPT_API std::uint32_t aether_physics_create_rope(std::uint32_t id, std::uint32_t targetId, Vec3 worldAnchor, float restLength)
{
	return SafeExport([&] -> std::uint32_t
	{
	auto* phys = ActiveContext().physics;
	if (phys == nullptr || !EntityAlive(id) || !CanControl(id))
	{
		return 0;
	}
	if (targetId != 0 && !EntityAlive(targetId))
	{
		return 0;
	}
	return phys->CreateDistanceConstraint(ActiveWorld(), aether::Entity{id}, aether::Entity{targetId}, ToGlm(worldAnchor), restLength);
	});
}

// Ownership is not re-checked here: a handle is meaningless to anyone who was not
// handed it in the first place (there is no discoverable "list of constraint
// handles" a hostile caller could enumerate), and DestroyConstraint on an unknown or
// already-cleaned-up handle is already a documented no-op - see PhysicsSystem.hpp.
AE_SCRIPT_API void aether_physics_destroy_constraint(std::uint32_t handle)
{
	SafeExport([&] -> void
	{
	auto* phys = ActiveContext().physics;
	if (phys == nullptr)
	{
		return;
	}
	phys->DestroyConstraint(ActiveWorld(), handle);
	});
}
