#include "audio/AudioSystem.hpp"

#include <algorithm>
#include <unordered_map>

#include <glm/geometric.hpp>

#include "audio/AudioSubsystem.hpp"
#include "camera/CameraManager.hpp"
#include "scene/AudioComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::audio
{
	namespace
	{
		// World position of an entity: scene transforms are world-space by
		// invariant (AGENTS.md), so the translation column IS the position.
		glm::vec3 WorldPositionOf(World& world, Entity entity)
		{
			if (const auto* transform = world.TryGet<TransformComponent>(entity))
			{
				return glm::vec3(transform->localToWorld[3]);
			}
			return glm::vec3{0.0f};
		}
	} // namespace

	AudioSystem::AudioSystem(ServiceContainer& services)
	      : m_audio(services.TryGet<audio::AudioSubsystem>())
	      , m_cameras(services.TryGet<CameraManager>())
	{
		// UiShell/Launcher runs no audio subsystem and no cameras; a scene app on
		// such a profile must simply stay silent, never fail.
	}

	void AudioSystem::Update(World& world, float dt)
	{
		if (m_audio == nullptr)
		{
			return;
		}
		const float frameDt = glm::max(dt, 1e-4f);

		// Listener follows the main camera, with its world velocity for the
		// doppler effect (position delta per frame). Game thread only.
		ListenerPose pose;
#if defined(AETHERCORE_WEB_RUNTIME)
		for (const auto entity : world.View<CameraComponent, TransformComponent, MainCameraComponent>())
		{
			const auto& transform = world.GetRegistry().get<TransformComponent>(entity);
			pose.position = glm::vec3(transform.localToWorld[3]);
			const glm::vec3 forward = -glm::vec3(transform.localToWorld[2]);
			if (glm::length(forward) > 1e-6f) pose.forward = glm::normalize(forward);
			pose.up = glm::cross(glm::cross(pose.forward, glm::vec3{0.0f, 1.0f, 0.0f}), pose.forward);
			pose.velocity = (pose.position - m_lastListenerPosition) / frameDt;
			m_lastListenerPosition = pose.position;
			break;
		}
#else
		if (const Camera* camera = m_cameras != nullptr ? m_cameras->TryGetMainCamera() : nullptr)
		{
			pose.position = camera->GetPosition();
			pose.forward = camera->GetForward();
			pose.up = glm::cross(glm::cross(pose.forward, glm::vec3{0.0f, 1.0f, 0.0f}), pose.forward);
			pose.velocity = (pose.position - m_lastListenerPosition) / frameDt;
			m_lastListenerPosition = pose.position;
		}
#endif
		m_audio->Update(dt, pose);

		// Keep an empty entry after completion so playOnStart is consumed only
		// once. Detaching or destroying the source releases its voice and entry.
		for (auto it = m_entityVoices.begin(); it != m_entityVoices.end();)
		{
			const auto entity = World::ToEntt(Entity{it->first});
			if (!world.GetRegistry().valid(entity) || !world.GetRegistry().all_of<AudioSourceComponent>(entity))
			{
				m_audio->StopVoice(it->second);
				m_lastEntityPositions.erase(it->first);
				it = m_entityVoices.erase(it);
			}
			else
			{
				if (!m_audio->IsVoiceAlive(it->second))
				{
					it->second = {};
					m_lastEntityPositions.erase(it->first);
				}
				++it;
			}
		}

		for (const auto [entity, source]: world.View<AudioSourceComponent>().each())
		{
			const std::uint32_t id = World::FromEntt(entity).id;
			const auto [entry, firstSeen] = m_entityVoices.try_emplace(id);
			const bool requested = source.playRequested;
			source.playRequested = false;
			if (requested && entry->second.IsValid())
			{
				m_audio->StopVoice(entry->second);
				entry->second = {};
			}
			const bool hasVoice = entry->second.IsValid();
			const glm::vec3 position = WorldPositionOf(world, World::FromEntt(entity));
			if ((!requested && !(firstSeen && source.playOnStart)) || hasVoice || source.clipPath.empty())
			{
				// Already playing, not auto-started, or authored silent: still
				// feed the spatializer the emitter's motion each frame.
				if (hasVoice && source.spatial)
				{
					glm::vec3 velocity{0.0f};
					if (const auto last = m_lastEntityPositions.find(id); last != m_lastEntityPositions.end())
					{
						velocity = (position - last->second) / frameDt;
					}
					m_lastEntityPositions[id] = position;
					m_audio->SetVoicePosition(m_entityVoices[id], position);
					m_audio->SetVoiceVelocity(m_entityVoices[id], velocity);
					// Periodic one-line proof of where the spatializer's inputs
					// are: relative position (x sign = left/right of the
					// listener), distance, and the doppler-driving velocity.
					// Dev builds only - a shipped game must not spam its log.
#if defined(AE_CONFIG_DEBUG) || defined(AE_CONFIG_DEV)
					if ((m_frames++ % 120) == 0)
					{
						const glm::vec3 rel = position - pose.position;
						AE_INFO(LogCategory::Audio, "spatial emitter {} rel=({:+.2f}, {:+.2f}, {:+.2f}) dist={:.2f} vel=({:+.2f}, {:+.2f}, {:+.2f})",
						        id, rel.x, rel.y, rel.z, glm::length(rel), velocity.x, velocity.y, velocity.z);
					}
#else
					++m_frames;
#endif
				}
				continue;
			}

			const PlayParams params{
			        .volume = source.volume,
			        .pitch = source.pitch,
			        .loop = source.loop,
			        .bus = static_cast<Bus>(source.bus),
			        .minDistance = source.minDistance,
			        .maxDistance = source.maxDistance,
			        .attenuationModel = source.attenuationModel,
			        .rolloff = source.rolloff,
			        .coneInnerDegrees = source.coneInnerDegrees,
			        .coneOuterDegrees = source.coneOuterDegrees,
			        .coneOuterVolume = source.coneOuterVolume,
			        .dopplerFactor = source.dopplerFactor,
			};
			const VoiceHandle handle = source.spatial
			        ? m_audio->Play3D(source.clipPath, position, params)
			        : m_audio->Play2D(source.clipPath, params);
			if (handle.IsValid())
			{
				m_entityVoices[id] = handle;
				m_lastEntityPositions[id] = position;
			}
		}
	}

	void AudioSystem::OnUnregister([[maybe_unused]] World& world)
	{
		// Stop owned voices before releasing their entity bookkeeping.
		if (m_audio != nullptr)
		{
			for (const auto& [id, voice]: m_entityVoices)
				m_audio->StopVoice(voice);
		}
		m_entityVoices.clear();
		m_lastEntityPositions.clear();
	}
} // namespace aether::audio
