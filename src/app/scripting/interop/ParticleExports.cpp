#include "scripting/interop/InteropCommon.hpp"

#include "particles/ParticleComponents.hpp"
#include "scene/World.hpp"

using namespace aether::app::scripting::interop;

namespace
{
	aether::ParticleEmitterComponent* Emitter(std::uint32_t id)
	{
		return ActiveWorld().TryGet<aether::ParticleEmitterComponent>(aether::Entity{id});
	}
} // namespace

// Queue a one-off burst of `count` particles on the entity's emitter. Consumed
// by ParticleSystem::Update later this frame.
AE_SCRIPT_API void aether_particles_burst(std::uint32_t id, std::int32_t count)
{
	SafeExport([&] -> void
	{
	if (auto* emitter = Emitter(id); emitter != nullptr && count > 0)
	{
		emitter->pendingBurst += static_cast<std::uint32_t>(count);
	}
	});
}

// Toggle continuous emission (the 'rate' stream); bursts are unaffected.
AE_SCRIPT_API void aether_particles_set_emitting(std::uint32_t id, std::int32_t on)
{
	SafeExport([&] -> void
	{
	if (auto* emitter = Emitter(id))
	{
		emitter->emitting = on != 0;
	}
	});
}
