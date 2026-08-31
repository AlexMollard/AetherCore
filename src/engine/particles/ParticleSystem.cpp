#include "particles/ParticleSystem.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <unordered_map>

#include "material/TextureRegistry.hpp"
#include "particles/ParticleComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		constexpr std::uint32_t kInvalidTextureSlot = 0xFFFFFFFFu;

		// Cheap xorshift so each emitter is independent and needs no global RNG.
		[[nodiscard]] float NextFloat(std::uint32_t& state) noexcept
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return static_cast<float>(state & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
		}

		[[nodiscard]] float Range(std::uint32_t& state, float lo, float hi) noexcept
		{
			return lo + (hi - lo) * NextFloat(state);
		}

		[[nodiscard]] std::uint32_t BiasSigned(std::int32_t value) noexcept
		{
			return static_cast<std::uint32_t>(std::clamp(value, -32768, 32767) + 32768);
		}

		[[nodiscard]] std::uint64_t MakeSortKey(std::int32_t layer, std::int32_t order, std::uint32_t id) noexcept
		{
			return (static_cast<std::uint64_t>(BiasSigned(layer)) << 48u) | (static_cast<std::uint64_t>(BiasSigned(order)) << 32u) | static_cast<std::uint64_t>(id);
		}

		void SpawnOne(ParticleEmitterComponent& emitter, glm::vec2 origin)
		{
			if (emitter.particles.size() >= emitter.maxParticles)
			{
				return;
			}
			const float angle = glm::radians(emitter.directionDeg + Range(emitter.rngState, -emitter.spreadDeg, emitter.spreadDeg));
			const float speed = Range(emitter.rngState, emitter.speedMin, emitter.speedMax);
			Particle p;
			p.position = origin;
			p.velocity = glm::vec2{std::cos(angle), std::sin(angle)} * speed;
			p.age = 0.0f;
			p.lifetime = std::max(0.01f, Range(emitter.rngState, emitter.lifetimeMin, emitter.lifetimeMax));
			p.sizeJitter = Range(emitter.rngState, 0.75f, 1.25f);
			emitter.particles.push_back(p);
		}

		// Effective collision radius: authored, else half the current visual size.
		[[nodiscard]] float CollisionRadius(const ParticleEmitterComponent& emitter, const Particle& p) noexcept
		{
			if (emitter.collisionRadius > 0.0f)
			{
				return emitter.collisionRadius;
			}
			const float t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
			return std::max(0.02f, glm::mix(emitter.startSize, emitter.endSize, t) * p.sizeJitter * 0.5f);
		}

		// Advance one particle, bouncing off physics colliders if requested.
		void IntegrateParticle(ParticleEmitterComponent& emitter, Particle& p, float dt, const Physics2DSystem* physics)
		{
			p.velocity += emitter.gravity * dt;
			glm::vec2 delta = p.velocity * dt;

			if (emitter.collideWorld && physics != nullptr)
			{
				const float dist = glm::length(delta);
				const float radius = CollisionRadius(emitter, p);
				if (dist > 1e-5f)
				{
					const glm::vec2 dir = delta / dist;
					const Physics2DSystem::RayHit2D hit = physics->CastRay(p.position, dir, dist + radius);
					if (hit.hit)
					{
						// Rest just off the surface, then reflect the inbound
						// normal component (bounce) and shed some tangential
						// speed (friction).
						p.position = hit.point + hit.normal * radius;
						const float vn = glm::dot(p.velocity, hit.normal);
						if (vn < 0.0f)
						{
							const glm::vec2 normalVel = vn * hit.normal;
							const glm::vec2 tangentVel = p.velocity - normalVel;
							p.velocity = tangentVel * (1.0f - emitter.collisionDamping) - normalVel * emitter.bounce;
						}
						return;
					}
				}
			}
			p.position += delta;
		}

		// Resolve sibling particles as equal-mass circles via a uniform hash grid.
		void ResolveParticleCollisions(ParticleEmitterComponent& emitter)
		{
			const std::size_t count = emitter.particles.size();
			if (count < 2)
			{
				return;
			}
			// Cell = 2x the largest radius so any overlapping pair shares or
			// neighbours a cell.
			float maxRadius = 0.0f;
			for (const Particle& p: emitter.particles)
			{
				maxRadius = std::max(maxRadius, CollisionRadius(emitter, p));
			}
			const float cell = std::max(0.05f, maxRadius * 2.0f);
			std::unordered_map<std::int64_t, std::vector<std::uint32_t>> grid;
			grid.reserve(count);
			const auto key = [cell](glm::vec2 pos) -> std::int64_t
			{
				const std::int32_t cx = static_cast<std::int32_t>(std::floor(pos.x / cell));
				const std::int32_t cy = static_cast<std::int32_t>(std::floor(pos.y / cell));
				return (static_cast<std::int64_t>(cx) << 32) ^ (static_cast<std::int64_t>(cy) & 0xFFFFFFFF);
			};
			for (std::uint32_t i = 0; i < count; ++i)
			{
				grid[key(emitter.particles[i].position)].push_back(i);
			}

			for (std::uint32_t i = 0; i < count; ++i)
			{
				Particle& a = emitter.particles[i];
				const std::int32_t cx = static_cast<std::int32_t>(std::floor(a.position.x / cell));
				const std::int32_t cy = static_cast<std::int32_t>(std::floor(a.position.y / cell));
				for (std::int32_t ox = -1; ox <= 1; ++ox)
				{
					for (std::int32_t oy = -1; oy <= 1; ++oy)
					{
						const std::int64_t k = (static_cast<std::int64_t>(cx + ox) << 32) ^ (static_cast<std::int64_t>(cy + oy) & 0xFFFFFFFF);
						const auto it = grid.find(k);
						if (it == grid.end())
						{
							continue;
						}
						for (const std::uint32_t j: it->second)
						{
							if (j <= i)
							{
								continue;
							}
							Particle& b = emitter.particles[j];
							const float minDist = CollisionRadius(emitter, a) + CollisionRadius(emitter, b);
							glm::vec2 d = a.position - b.position;
							float distSq = glm::dot(d, d);
							if (distSq >= minDist * minDist || distSq < 1e-8f)
							{
								continue;
							}
							const float dist = std::sqrt(distSq);
							const glm::vec2 n = d / dist;
							const float overlap = minDist - dist;
							a.position += n * (overlap * 0.5f);
							b.position -= n * (overlap * 0.5f);
							const float relVel = glm::dot(a.velocity - b.velocity, n);
							if (relVel < 0.0f)
							{
								const float impulse = -(1.0f + emitter.bounce) * relVel * 0.5f;
								a.velocity += n * impulse;
								b.velocity -= n * impulse;
							}
						}
					}
				}
			}
		}
	} // namespace

	void ParticleSystem::QueueBurst(ParticleEmitterComponent& emitter, std::uint32_t count)
	{
		emitter.pendingBurst += count;
	}

	// The shared per-emitter step: start burst, queued bursts, continuous emission,
	// integrate + swap-pop cull, then sibling collisions. `physics` is null for the
	// editor preview (no world collision).
	void ParticleSystem::StepEmitter(ParticleEmitterComponent& emitter, float dt, glm::vec2 origin, const Physics2DSystem* physics)
	{
		if (emitter.emitOnStart && !emitter.started)
		{
			emitter.pendingBurst += emitter.burstCount;
		}
		emitter.started = true;

		while (emitter.pendingBurst > 0)
		{
			SpawnOne(emitter, origin);
			--emitter.pendingBurst;
		}

		if (emitter.emitting && emitter.rate > 0.0f)
		{
			emitter.spawnAccumulator += emitter.rate * dt;
			while (emitter.spawnAccumulator >= 1.0f)
			{
				SpawnOne(emitter, origin);
				emitter.spawnAccumulator -= 1.0f;
			}
		}

		for (std::size_t i = 0; i < emitter.particles.size();)
		{
			Particle& p = emitter.particles[i];
			p.age += dt;
			if (p.age >= p.lifetime)
			{
				emitter.particles[i] = emitter.particles.back();
				emitter.particles.pop_back();
				continue;
			}
			IntegrateParticle(emitter, p, dt, physics);
			++i;
		}

		if (emitter.collideParticles)
		{
			ResolveParticleCollisions(emitter);
		}
	}

	void ParticleSystem::StepStandalone(ParticleEmitterComponent& emitter, float dt, glm::vec2 origin)
	{
		if (dt <= 0.0f)
		{
			return;
		}
		if (emitter.rngState == 0)
		{
			emitter.rngState = 0x9E3779B9u;
		}
		StepEmitter(emitter, dt, origin, nullptr); // no world collision in preview
	}

	void ParticleSystem::Update(World& world, float dt)
	{
		Simulate(world, dt, /*retireFinished=*/true);
	}

	void ParticleSystem::UpdatePreview(World& world, float dt)
	{
		Simulate(world, dt, /*retireFinished=*/false);
	}

	void ParticleSystem::Simulate(World& world, float dt, bool retireFinished)
	{
		AE_PROFILE_ZONE();
		if (dt <= 0.0f)
		{
			dt = 0.0f;
		}

		// World-collision particles raycast against the current physics world,
		// which Physics2DSystem has already stepped this frame.
		const auto* physics = static_cast<const Physics2DSystem*>(world.FindSystem("Physics2DSystem"));

		std::vector<Entity> toDestroy;
		for (auto&& [raw, emitter, transform]: world.GetRegistry().view<ParticleEmitterComponent, TransformComponent>().each())
		{
			const Entity entity = World::FromEntt(raw);
			if (ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			if (emitter.rngState == 0)
			{
				emitter.rngState = (entity.id * 2654435761u) | 1u;
			}
			const glm::vec2 origin{transform.localToWorld[3].x, transform.localToWorld[3].y};

			StepEmitter(emitter, dt, origin, physics);

			// One-shot emitter that has finished: retire its entity.
			const bool idle = emitter.particles.empty() && emitter.pendingBurst == 0 && !(emitter.emitting && emitter.rate > 0.0f);
			if (retireFinished && emitter.autoDestroyWhenDone && emitter.started && idle)
			{
				toDestroy.push_back(entity);
			}
		}

		for (const Entity e: toDestroy)
		{
			world.Destroy(e);
		}
	}

	TextureHandle ParticleSystem::ResolveTexture(const std::string& path)
	{
		if (path.empty())
		{
			return m_textures.WhiteHandle();
		}
		if (const auto it = m_textureCache.find(path); it != m_textureCache.end())
		{
			return it->second;
		}
		const TextureHandle handle = m_textures.Acquire(path);
		m_textureCache.emplace(path, handle);
		return handle;
	}

	void ParticleSystem::Extract(World& world, Render2DFrameData& output)
	{
		AE_PROFILE_ZONE();
		for (auto&& [raw, emitter, transform]: world.GetRegistry().view<ParticleEmitterComponent, TransformComponent>().each())
		{
			const Entity entity = World::FromEntt(raw);
			if (emitter.particles.empty() || ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			TextureHandle texture = ResolveTexture(emitter.texturePath);
			std::uint32_t slot = m_textures.ResolveSlot(texture);
			if (slot == kInvalidTextureSlot)
			{
				slot = m_textures.ResolveSlot(m_textures.WhiteHandle());
			}
			const float z = transform.localToWorld[3].z;
			const std::uint64_t sortKey = MakeSortKey(emitter.sortingLayer, emitter.orderInLayer, entity.id);

			for (const Particle& p: emitter.particles)
			{
				const float t = std::clamp(p.age / p.lifetime, 0.0f, 1.0f);
				const float size = std::max(0.0f, glm::mix(emitter.startSize, emitter.endSize, t)) * p.sizeJitter;
				const glm::vec4 color = glm::mix(emitter.startColor, emitter.endColor, t);

				glm::mat4 worldMat(1.0f);
				worldMat[3] = glm::vec4(p.position.x, p.position.y, z, 1.0f);

				output.sprites.push_back(SpriteRenderInstance{
				        .world = worldMat,
				        .uvRect = {0.0f, 0.0f, 1.0f, 1.0f},
				        .color = color,
				        .sizeAndPivot = {size, size, 0.5f, 0.5f},
				        .sortKey = sortKey,
				        .textureIndex = slot,
				        .entityId = entity.id,
				        .flags = SpriteInstanceFlags::None,
				        .blendMode = static_cast<std::uint32_t>(emitter.blendMode),
				});
			}
		}
	}

	void ParticleSystem::Shutdown()
	{
		for (const auto& [path, handle]: m_textureCache)
		{
			(void) path;
			m_textures.Release(handle);
		}
		m_textureCache.clear();
	}
} // namespace aether
