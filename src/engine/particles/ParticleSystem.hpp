#pragma once

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include "material/TextureHandle.hpp"
#include "scene/System.hpp"

namespace aether
{
	class World;
	class TextureRegistry;
	struct Render2DFrameData;
	struct ParticleEmitterComponent;

	// Simulates ParticleEmitterComponents (game thread, via Update) and emits
	// live particles into the 2D sprite stream (via Extract, at frame prep).
	class ParticleSystem final : public System
	{
	public:
		explicit ParticleSystem(TextureRegistry& textures)
		      : m_textures(textures)
		{
		}

		[[nodiscard]] const char* GetName() const override
		{
			return "ParticleSystem";
		}

		void Update(World& world, float dt) override;

		void OnUnregister(World& world) override
		{
			(void) world;
			Shutdown();
		}

		// Append every live particle as a textured quad instance. Safe to call
		// from frame extraction; reads only ECS + the texture registry.
		void Extract(World& world, Render2DFrameData& output);

		void Shutdown();

		// Script-facing: queue a burst / toggle continuous emission.
		static void QueueBurst(ParticleEmitterComponent& emitter, std::uint32_t count);

		// Advance a standalone emitter with no ECS/physics dependency, for an
		// in-editor preview. Spawns from `origin`, integrates + culls, resolves
		// sibling collisions, but never touches the world or auto-destroys.
		static void StepStandalone(ParticleEmitterComponent& emitter, float dt, glm::vec2 origin);

	private:
		TextureHandle ResolveTexture(const std::string& path);

		TextureRegistry& m_textures;
		std::unordered_map<std::string, TextureHandle> m_textureCache;
	};
} // namespace aether
