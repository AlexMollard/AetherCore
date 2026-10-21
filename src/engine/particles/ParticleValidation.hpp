#pragma once

#include <cmath>
#include <expected>
#include <string>

#include "particles/ParticleComponents.hpp"

namespace aether
{
	inline constexpr std::uint32_t kWebParticleCapacity = 65536;

	inline std::expected<void, std::string> ValidateBrowserParticleEmitter(const ParticleEmitterComponent& emitter)
	{
		if (emitter.maxParticles > kWebParticleCapacity || emitter.particles.size() > emitter.maxParticles || emitter.burstCount > kWebParticleCapacity || emitter.pendingBurst > kWebParticleCapacity)
		{
			return std::unexpected("Browser particle capacity/burst limit exceeded (65536)");
		}
		for (float value: {emitter.rate,
		             emitter.emitDuration,
		             emitter.lifetimeMin,
		             emitter.lifetimeMax,
		             emitter.speedMin,
		             emitter.speedMax,
		             emitter.directionDeg,
		             emitter.spreadDeg,
		             emitter.startSize,
		             emitter.endSize,
		             emitter.rotationJitterDeg,
		             emitter.radialSpeed,
		             emitter.bounce,
		             emitter.collisionDamping,
		             emitter.collisionRadius,
		             emitter.spawnAccumulator,
		             emitter.emitElapsed})
		{
			if (!std::isfinite(value))
			{
				return std::unexpected("Non-finite browser particle emitter field");
			}
		}
		const auto finite = [](auto vector)
		{
			for (glm::length_t i = 0; i < vector.length(); ++i)
			{
				if (!std::isfinite(vector[i]))
				{
					return false;
				}
			}
			return true;
		};
		if (!finite(emitter.gravity) || !finite(emitter.velocity3D) || !finite(emitter.velocityJitter) || !finite(emitter.spawnJitter) || !finite(emitter.gravity3D) || !finite(emitter.startColor) || !finite(emitter.endColor)
		        || !finite(emitter.uvRect))
		{
			return std::unexpected("Non-finite browser particle vector");
		}
		if (emitter.rate < 0.0f || emitter.rate > kWebParticleCapacity || emitter.spawnAccumulator < 0.0f || emitter.spawnAccumulator > kWebParticleCapacity || emitter.lifetimeMin <= 0.0f || emitter.lifetimeMax < emitter.lifetimeMin
		        || emitter.space > ParticleSpace::Billboard3D || emitter.emitShape > ParticleEmitShape::ImprovedRadial)
		{
			return std::unexpected("Invalid browser particle lifetime, rate, space or emission shape");
		}
		const auto keys = [&](const auto& values, auto read)
		{
			if (values.size() > 16)
			{
				return false;
			}
			float previous = 0.0f;
			for (const auto& value: values)
			{
				if (!std::isfinite(value.t) || value.t < previous || value.t > 1.0f || !read(value))
				{
					return false;
				}
				previous = value.t;
			}
			return true;
		};
		const auto scalar = [](const auto& value)
		{
			return std::isfinite(value.value);
		};
		if (!keys(emitter.colorKeys, [&](const auto& value) { return finite(value.color); }) || !keys(emitter.alphaKeys, scalar) || !keys(emitter.sizeKeys, scalar) || !keys(emitter.rotationKeys, scalar))
		{
			return std::unexpected("Invalid browser particle keys (finite, ordered normalized time, at most 16)");
		}
		for (const auto& particle: emitter.particles)
		{
			if (!finite(particle.position) || !finite(particle.velocity) || !finite(particle.position3D) || !finite(particle.velocity3D) || !std::isfinite(particle.age) || !std::isfinite(particle.lifetime) || !std::isfinite(particle.sizeJitter)
			        || !std::isfinite(particle.rotationOffsetDeg))
			{
				return std::unexpected("Non-finite live browser particle");
			}
		}
		return {};
	}
} // namespace aether
