#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "scene/World.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "scripting/SceneContext.hpp"

namespace
{
	using namespace aether::app::scripting;

	// Access the physics system via the active script context (consistent with
	// every other module). The previous file-scope static s_physicsSystem was
	// the one outlier that bypassed the TLS SceneContext path.
	[[nodiscard]] inline aether::PhysicsSystem* active_physics()
	{
		auto& ctx = ActiveContext();
		return ctx.physics;
	}

	// set_linear_velocity(world, entity_id, x, y, z)
	void das_set_linear_velocity(aether::World* w, uint32_t id, float x, float y, float z)
	{
		auto* phys = active_physics();
		if (!phys)
		{
			return;
		}
		const auto rb = w->TryGet<aether::RigidBodyComponent>(aether::Entity{id});
		if (!rb)
		{
			return;
		}
		phys->SetLinearVelocity(rb->bodyId, {x, y, z});
	}

	// get_linear_velocity(world, entity_id) -> float3
	das::float3 das_get_linear_velocity(aether::World* w, uint32_t id)
	{
		if (auto* phys = active_physics())
		{
			if (const auto rb = w->TryGet<aether::RigidBodyComponent>(aether::Entity{id}))
			{
				return to_das(phys->GetLinearVelocity(rb->bodyId));
			}
		}
		return {0.f, 0.f, 0.f};
	}

	// add_box_body(world, entity_id, half_extents, dynamic)
	void das_add_box_body(aether::World* w, uint32_t id, das::float3 half, bool dynamic)
	{
		aether::BoxBodyDesc desc{};
		desc.halfExtents = to_glm(half);
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
		const auto ps = w->TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
		return ps ? to_das(ps->currPosition) : das::float3{};
	}

	// get_physics_scale(world, entity_id) -> float3
	das::float3 das_get_physics_scale(aether::World* w, uint32_t id)
	{
		const auto ps = w->TryGet<aether::PhysicsStateComponent>(aether::Entity{id});
		return ps ? to_das(ps->scale) : das::float3{1.0f, 1.0f, 1.0f};
	}

	// set_physics_debug_enabled(enabled: bool)
	void das_set_physics_debug_enabled(bool enabled)
	{
		aether::SetDebugRenderingEnabled(enabled);
	}

	// is_physics_debug_enabled() -> bool
	bool das_is_physics_debug_enabled()
	{
		return aether::IsDebugRenderingEnabled();
	}

} // namespace

// -- Module --------------------------------------------------------------------

namespace aether::app::scripting
{
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