#pragma once

#include "scene/System.hpp"

#include <glm/glm.hpp>
#include <unordered_map>

namespace aether
{
	class ServiceContainer;
	class CameraManager;

	namespace audio
	{
		class AudioSubsystem;
		struct VoiceHandle;
		struct ListenerPose;
	}

	namespace audio
	{
		// Drives audio from the ECS, on the game thread:
		//   - publishes the main camera's pose as the audio listener;
		//   - starts AudioSourceComponents flagged playOnStart the first frame they
		//     exist (and once per play session - the voice map is keyed by entity
		//     generation-safe handle and cleared from OnUnregister);
		//   - follows moving emitters and refreshes 3D gain/pan through
		//     AudioSubsystem::Update.
		//
		// Registered by Application like the other scene systems; because the editor
		// only ticks World systems while playing, edit mode never starts audio and
		// stop/pause go through PlaySession's explicit StopAllVoices/SetSuspended.
		class AudioSystem : public System
		{
		public:
			explicit AudioSystem(ServiceContainer& services);

			void Update(World& world, float dt) override;
			void OnUnregister(World& world) override;

			[[nodiscard]] const char* GetName() const override
			{
				return "AudioSystem";
			}

		private:
			AudioSubsystem* m_audio = nullptr;
			CameraManager* m_cameras = nullptr;
			// entity -> voice currently playing its component's clip.
			std::unordered_map<std::uint32_t, VoiceHandle> m_entityVoices;
			// entity -> last world position, for the per-frame doppler velocity.
			std::unordered_map<std::uint32_t, glm::vec3> m_lastEntityPositions;
			glm::vec3 m_lastListenerPosition{0.0f};
			std::uint64_t m_frames = 0;
		};
	} // namespace audio
} // namespace aether
