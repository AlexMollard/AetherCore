#include "scripting/interop/InteropCommon.hpp"

#include "net/NetworkContext.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	aether::net::NetworkContext* Context()
	{
		const auto& ctx = ActiveContext();
		if (ctx.services == nullptr)
		{
			return nullptr;
		}
		return ctx.services->TryGet<aether::net::NetworkContext>();
	}

	// Character.Move/SetVelocity/Jump write CharacterControllerComponent's script-input
	// fields, which StepCharacters later turns into actual motion (see
	// CharacterControllerComponent::locallySimulated). Nothing gated this before: a
	// script could call Move on ANY entity, not just ones it owns - the same gap
	// RigidBodyComponent still has, found here first because this file had no
	// ownership check at all rather than a wrong one. locallySimulated already stops a
	// non-owner's StepCharacters from integrating the result, but relying on that
	// alone leaves the field itself writable by anyone; a caller must not be able to
	// drive another peer's character even in the window before that peer's own
	// authority sync runs. HasAuthority is offline-safe (returns true with no
	// NetworkContext registered) so single-player pays nothing for this check.
	bool CanControl(std::uint32_t id)
	{
		const aether::net::NetworkContext* context = Context();
		return context == nullptr || context->HasAuthority(ActiveWorld(), aether::Entity{id});
	}
} // namespace

AE_SCRIPT_API void aether_character_add(std::uint32_t id, float radius, float halfHeight)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(id))
	{
		return;
	}
	ActiveWorld().EmplaceOrReplace<aether::CharacterControllerComponent>(aether::Entity{id}, aether::CharacterControllerComponent{.radius = radius, .halfHeight = halfHeight});
	});
}

// Sets the character's desired HORIZONTAL velocity (direction * speed); any vertical
// component the caller passes is stripped by StepCharacters, which owns the vertical
// velocity (gravity, ground-follow, jump). Persists frame to frame, exactly like
// RigidBodyComponent's velocity, until the next Move or SetVelocity call.
AE_SCRIPT_API void aether_character_move(std::uint32_t id, Vec3 direction, float speed)
{
	SafeExport([&] -> void
	{
	auto* cc = ActiveWorld().TryGet<aether::CharacterControllerComponent>(aether::Entity{id});
	if (cc == nullptr || !CanControl(id))
	{
		return;
	}
	cc->desiredVelocity = ToGlm(direction) * speed;
	cc->velocityOverride = false;
	});
}

// Raw full velocity override (both horizontal and vertical) - the character-controller
// counterpart of Physics.SetLinearVelocity, for scripts that want to fully own the
// character's motion for a step (knockback, a scripted cutscene move, ...).
AE_SCRIPT_API void aether_character_set_velocity(std::uint32_t id, Vec3 velocity)
{
	SafeExport([&] -> void
	{
	auto* cc = ActiveWorld().TryGet<aether::CharacterControllerComponent>(aether::Entity{id});
	if (cc == nullptr || !CanControl(id))
	{
		return;
	}
	cc->desiredVelocity = ToGlm(velocity);
	cc->velocityOverride = true;
	});
}

AE_SCRIPT_API Vec3 aether_character_get_velocity(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	const auto* cc = ActiveWorld().TryGet<aether::CharacterControllerComponent>(aether::Entity{id});
	return cc != nullptr ? FromGlm(cc->velocity) : Vec3{};
	});
}

// One-shot: applied on the next substep if (and only if) the character is grounded at
// that moment, then dropped. No jump buffering/coyote time - see StepCharacters.
AE_SCRIPT_API void aether_character_jump(std::uint32_t id, float speed)
{
	SafeExport([&] -> void
	{
	auto* cc = ActiveWorld().TryGet<aether::CharacterControllerComponent>(aether::Entity{id});
	if (cc == nullptr || !CanControl(id))
	{
		return;
	}
	cc->pendingJumpSpeed = speed;
	});
}

AE_SCRIPT_API std::int32_t aether_character_is_grounded(std::uint32_t id)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* cc = ActiveWorld().TryGet<aether::CharacterControllerComponent>(aether::Entity{id});
	return (cc != nullptr && cc->isGrounded) ? 1 : 0;
	});
}

AE_SCRIPT_API Vec3 aether_character_get_ground_normal(std::uint32_t id)
{
	return SafeExport([&] -> Vec3
	{
	const auto* cc = ActiveWorld().TryGet<aether::CharacterControllerComponent>(aether::Entity{id});
	return cc != nullptr ? FromGlm(cc->groundNormal) : Vec3{0.0f, 1.0f, 0.0f};
	});
}

AE_SCRIPT_API std::uint32_t aether_character_get_ground_entity(std::uint32_t id)
{
	return SafeExport([&] -> std::uint32_t
	{
	const auto* cc = ActiveWorld().TryGet<aether::CharacterControllerComponent>(aether::Entity{id});
	return cc != nullptr ? cc->groundEntity : 0u;
	});
}
