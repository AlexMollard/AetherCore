#pragma once

#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include "material/TextureHandle.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "scene/System.hpp"

namespace aether
{
	class World;
	class TextureRegistry;
	class Physics2DSystem;
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
		// The same simulation WITHOUT retiring finished emitters. Edit mode previews effects so
		// they can be authored, and an emitter with autoDestroyWhenDone is finished the moment
		// it is added - so running the runtime path there deletes the entity out from under
		// whoever is building it, before the scene has ever been played.
		void UpdatePreview(World& world, float dt);

	private:
		void Simulate(World& world, float dt, bool retireFinished);

	public:

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

		// Frame prep hands over where extracted billboard particles go this frame. The pointer
		// is packet-owned and only read during Extract.
		void SetBillboardTarget(std::vector<BillboardParticleInstance>* out) { m_billboardOut = out; }

		// Advance a standalone emitter with no ECS/physics dependency, for an
		// in-editor preview. Spawns from `origin`, integrates + culls, resolves
		// sibling collisions, but never touches the world or auto-destroys.
		static void StepStandalone(ParticleEmitterComponent& emitter, float dt, glm::vec2 origin);

	private:
		// Shared spawn/integrate/cull step for one emitter (physics == null skips
		// world collision). Used by Update (per entity) and StepStandalone (preview).
		static void StepEmitter(ParticleEmitterComponent& emitter, float dt, glm::vec2 origin, glm::vec3 origin3D, const Physics2DSystem* physics);

		// Append one emitter's live 3D particles to the packet's billboard stream.
		void ExtractBillboards(const ParticleEmitterComponent& emitter, Entity entity);

		TextureHandle ResolveTexture(const std::string& path);

		TextureRegistry& m_textures;
		std::unordered_map<std::string, TextureHandle> m_textureCache;
		std::vector<BillboardParticleInstance>* m_billboardOut = nullptr;
	};
} // namespace aether
