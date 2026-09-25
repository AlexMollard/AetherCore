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

// Over-lifetime keys, evaluated at normalised particle age. Each key is (t, x, y, z):
// t in 0..1, then the value - rgb for channel 0 (colour), a scalar in x for the others.
// channel: 0 colour, 1 alpha, 2 size, 3 rotation (degrees).
AE_SCRIPT_API void aether_particles_set_keys(std::uint32_t id, std::int32_t channel, const Vec4* keys, std::int32_t count)
{
	SafeExport([&] -> void
	{
	auto* emitter = Emitter(id);
	if (emitter == nullptr || keys == nullptr || count <= 0)
	{
		return;
	}
	const auto read = [keys, count](auto make)
	{
		for (std::int32_t i = 0; i < count; ++i)
		{
			make(keys[i]);
		}
	};
	switch (channel)
	{
		case 0:
			emitter->colorKeys.clear();
			read([&](const Vec4& k) { emitter->colorKeys.push_back({k.x, glm::vec3(k.y, k.z, k.w)}); });
			break;
		case 1:
			emitter->alphaKeys.clear();
			read([&](const Vec4& k) { emitter->alphaKeys.push_back({k.x, k.y}); });
			break;
		case 2:
			emitter->sizeKeys.clear();
			read([&](const Vec4& k) { emitter->sizeKeys.push_back({k.x, k.y}); });
			break;
		case 3:
			emitter->rotationKeys.clear();
			read([&](const Vec4& k) { emitter->rotationKeys.push_back({k.x, k.y}); });
			break;
		default:
			break;
	}
	});
}
