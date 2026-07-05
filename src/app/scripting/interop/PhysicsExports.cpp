#include "scripting/interop/InteropCommon.hpp"

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/World.hpp"

// Physics control exported to C#. Bodies mirror the old daScript PhysicsModule:
// shape descriptors are emplaced for PhysicsSystem to consume, velocity/state go
// through the live rigid body.

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	aether::PhysicsMotionType MotionType(std::int32_t dynamic)
	{
		return dynamic != 0 ? aether::PhysicsMotionType::Dynamic : aether::PhysicsMotionType::Static;
	}
} // namespace

AE_SCRIPT_API void aether_physics_add_box(std::uint32_t id, Vec3 halfExtents, std::int32_t dynamic)
{
	aether::BoxBodyDesc desc{};
	desc.halfExtents = ToGlm(halfExtents);
	desc.motionType = MotionType(dynamic);
	ActiveWorld().Emplace<aether::BoxBodyDesc>(aether::Entity{id}, desc);
}

AE_SCRIPT_API void aether_physics_add_sphere(std::uint32_t id, float radius, std::int32_t dynamic)
{
	aether::SphereBodyDesc desc{};
	desc.radius = radius;
	desc.motionType = MotionType(dynamic);
	ActiveWorld().Emplace<aether::SphereBodyDesc>(aether::Entity{id}, desc);
}

AE_SCRIPT_API void aether_physics_add_capsule(std::uint32_t id, float halfHeight, float radius, std::int32_t dynamic)
{
	aether::CapsuleBodyDesc desc{};
	desc.halfHeight = halfHeight;
	desc.radius = radius;
	desc.motionType = MotionType(dynamic);
	ActiveWorld().Emplace<aether::CapsuleBodyDesc>(aether::Entity{id}, desc);
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
