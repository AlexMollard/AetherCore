#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "scene/World.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics/PhysicsDebugRenderer.hpp"

namespace
{
	aether::PhysicsSystem* s_physicsSystem = nullptr;

	// set_linear_velocity(world, entity_id, x, y, z)
	void das_set_linear_velocity(aether::World* w, uint32_t id, float x, float y, float z)
	{
		if (!s_physicsSystem)
		{
			return;
		}
		const auto* rb = w->TryGet<aether::RigidBodyComponent>(aether::Entity{id});
		if (!rb)
		{
			return;
		}
		s_physicsSystem->SetLinearVelocity(rb->bodyId, {x, y, z});
	}

	// get_linear_velocity(world, entity_id) -> float3
	das::float3 das_get_linear_velocity(aether::World* w, uint32_t id)
	{
		das::float3 r{0.f, 0.f, 0.f};
		if (!s_physicsSystem)
		{
			return r;
		}
		const auto* rb = w->TryGet<aether::RigidBodyComponent>(aether::Entity{id});
		if (!rb)
		{
			return r;
		}
		glm::vec3 vel = s_physicsSystem->GetLinearVelocity(rb->bodyId);
		r.x = vel.x;
		r.y = vel.y;
		r.z = vel.z;
		return r;
	}

	// add_box_body(world, entity_id, half_extents, dynamic)
	void das_add_box_body(aether::World* w, uint32_t id, das::float3 half, bool dynamic)
	{
		aether::BoxBodyDesc desc{};
		desc.halfExtents = {half.x, half.y, half.z};
		desc.motionType = dynamic ? aether::PhysicsMotionType::Dynamic : aether::PhysicsMotionType::Static;
		w->Emplace<aether::BoxBodyDesc>(aether::Entity{id}, desc);
	}

	// add_sphere_body(world, entity_id, radius, dynamic)
	void das_add_sphere_body(aether::World* w, uint32_t id, float radius, bool dynamic)
	{
		aether::SphereBodyDesc desc{};
		desc.radius = radius;
		desc.motionType = dynamic ? aether::PhysicsMotionType::Dynamic : aether::PhysicsMotionType::Static;
		w->Emplace<aether::SphereBodyDesc>(aether::Entity{id}, desc);
	}

	// add_capsule_body(world, entity_id, half_height, radius, dynamic)
	void das_add_capsule_body(aether::World* w, uint32_t id, float halfHeight, float radius, bool dynamic)
	{
		aether::CapsuleBodyDesc desc{};
		desc.halfHeight = halfHeight;
		desc.radius = radius;
		desc.motionType = dynamic ? aether::PhysicsMotionType::Dynamic : aether::PhysicsMotionType::Static;
		w->Emplace<aether::CapsuleBodyDesc>(aether::Entity{id}, desc);
	}

	// get_physics_position(world, entity_id) -> float3
	// Returns the interpolated world-space position from PhysicsStateComponent.
	das::float3 das_get_physics_position(aether::World* w, uint32_t id)
	{
		das::float3 r{};
		const auto* ps = w->TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
		if (!ps)
		{
			return r;
		}
		r.x = ps->currPosition.x;
		r.y = ps->currPosition.y;
		r.z = ps->currPosition.z;
		return r;
	}

	// get_physics_scale(world, entity_id) -> float3
	das::float3 das_get_physics_scale(aether::World* w, uint32_t id)
	{
		das::float3 r{};
		r.x = r.y = r.z = 1.0f;
		const auto* ps = w->TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
		if (!ps)
		{
			return r;
		}
		r.x = ps->scale.x;
		r.y = ps->scale.y;
		r.z = ps->scale.z;
		return r;
	}

	// set_physics_debug_enabled(enabled: bool)
	void das_set_physics_debug_enabled(bool enabled)
	{
		aether::SetPhysicsDebugRenderingEnabled(enabled);
	}

	// is_physics_debug_enabled() -> bool
	bool das_is_physics_debug_enabled()
	{
		return aether::IsPhysicsDebugRenderingEnabled();
	}

} // namespace

// ── Module ────────────────────────────────────────────────────────────────────

namespace aether::app::scripting
{
	void InitPhysicsModule(aether::PhysicsSystem* physics)
	{
		s_physicsSystem = physics;
	}

	struct PhysicsModule : DasModuleBase
	{
		PhysicsModule()
		      : DasModuleBase("physics")
		{
			das::ModuleLibrary lib(this);
			lib.addModule(das::Module::require("world")); // World* used in all bindings

			// Shape descriptor setup - typed wrappers set motionType etc.
			Bind<das_add_box_body>(lib, "add_box_body", SE::modifyExternal);
			Bind<das_add_sphere_body>(lib, "add_sphere_body", SE::modifyExternal);
			Bind<das_add_capsule_body>(lib, "add_capsule_body", SE::modifyExternal);

			// Descriptor structural ops (allows has/remove without re-creating)
			BIND_COMPONENT("box_body_desc", aether::BoxBodyDesc)
			BIND_COMPONENT("sphere_body_desc", aether::SphereBodyDesc)
			BIND_COMPONENT("capsule_body_desc", aether::CapsuleBodyDesc)

			// Runtime rigid body - structural ops only (managed by PhysicsSystem)
			BIND_COMPONENT("rigid_body", aether::RigidBodyComponent)

			// Physics state - structural ops + read-only position/scale access
			BIND_COMPONENT("physics_state", aether::PhysicsStateComponent)
			Bind<das_get_physics_position>(lib, "get_physics_position", SE::accessExternal);
			Bind<das_get_physics_scale>(lib, "get_physics_scale", SE::accessExternal);

			// Velocity control
			Bind<das_set_linear_velocity>(lib, "set_linear_velocity", SE::modifyExternal);
			Bind<das_get_linear_velocity>(lib, "get_linear_velocity", SE::accessExternal);

			// Physics debug visualization toggle
			Bind<das_set_physics_debug_enabled>(lib, "set_physics_debug_enabled", SE::modifyExternal);
			Bind<das_is_physics_debug_enabled>(lib, "is_physics_debug_enabled", SE::accessExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(PhysicsModule, aether::app::scripting)
