#pragma once

// The audio subsystem: a miniaudio device + resource manager routed through the
// engine's io::FileSystem, so project:// and engine:// audio (and therefore paks)
// just work. Game-thread only: every entry point here must be called from the
// thread that ticks World systems; miniaudio's own device thread mixes in the
// background, which is exactly the boundary the engine's render-thread rule wants.
//
// Backend: miniaudio was chosen because it is a single header with built-in
// WASAPI/ALSA/null backends and WAV/MP3/FLAC/Ogg-Vorbis decoding - no SDK, no
// system dependency, and the null backend lets the whole stack run headless.
//
// 3D: spatialisation is miniaudio's own spatializer - sounds carry position,
// velocity, cone and attenuation parameters, and the listener pose (position,
// direction, world-up, velocity for doppler) is published by the game thread
// each frame. audio/AudioMath.hpp owns only the validated mapping of authored
// component values into that API, which is what the unit tests cover.

// miniaudio types never appear in this header: several of them are typedef
// aliases (ma_sound_group is a typedef of ma_sound), so forward-declaring
// `struct` versions redefines them. Storage is pimpl + void*; the cpp includes
// miniaudio.h alone.
#include <cstdint>
#include <memory>
#include <string>

#include "audio/AudioMath.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	struct EngineSettings;
}

namespace aether::audio
{
	struct PlayParams
	{
		float volume = 1.0f;
		float pitch = 1.0f;
		bool loop = false;
		Bus bus = Bus::Sfx;
		// Spatial only; mapped straight into miniaudio's spatialiser.
		float minDistance = 1.0f;
		float maxDistance = 50.0f;
		// 0 inverse (default), 1 linear, 2 exponential (see AudioMath::AttenuationModelIndex).
		int attenuationModel = 0;
		float rolloff = 1.0f;
		// Directional cone in degrees about the emitter's direction (>= 360 = omni).
		float coneInnerDegrees = 360.0f;
		float coneOuterDegrees = 360.0f;
		float coneOuterVolume = 1.0f;
		// Doppler strength; 0 disables.
		float dopplerFactor = 1.0f;
	};

	struct VoiceHandle
	{
		std::uint32_t id = 0; // 0 = invalid

		[[nodiscard]] bool IsValid() const noexcept
		{
			return id != 0;
		}
	};

	struct ListenerPose
	{
		glm::vec3 position{0.0f};
		glm::vec3 forward{0.0f, 0.0f, -1.0f};
		glm::vec3 up{0.0f, 1.0f, 0.0f};
		// World velocity, for the doppler effect (position delta per frame).
		glm::vec3 velocity{0.0f};
	};

	struct Stats
	{
		std::uint32_t activeVoices = 0;
		std::uint32_t startedTotal = 0;
		bool musicPlaying = false;
	};

	class AudioSubsystem
	{
	public:
		AudioSubsystem();
		~AudioSubsystem();
		AudioSubsystem(const AudioSubsystem&) = delete;
		AudioSubsystem& operator=(const AudioSubsystem&) = delete;

		// Config.headless forces the null backend (tests, no-audio machines).
		struct Config
		{
			bool headless = false;
		};
		// Never throws: a machine with no audio device falls back to the null
		// backend (silent but functional), which is what keeps the engine
		// initialising on a server or CI runner.
		[[nodiscard]] Expected<void> Init(const Config& config = {});
		void Shutdown();

		[[nodiscard]] bool IsInitialized() const noexcept
		{
			return m_engine != nullptr;
		}

		// ── One-shot sounds ────────────────────────────────────────────────
		// Fully decoded on start (MA_SOUND_FLAG_NO_STREAM): SFX are small and
		// must never stall a play call on disk. Returns an invalid handle when
		// the clip is missing or undecodable - logged, never fatal.
		[[nodiscard]] VoiceHandle Play2D(std::string_view clipPath, const PlayParams& params = {});
		[[nodiscard]] VoiceHandle Play3D(std::string_view clipPath, const glm::vec3& position, const PlayParams& params = {});
		void StopVoice(VoiceHandle handle);
		void SetVoicePosition(VoiceHandle handle, const glm::vec3& position);
		void SetVoiceVelocity(VoiceHandle handle, const glm::vec3& velocity);
		// False once the voice finished (non-looping) or was stopped/stolen.
		[[nodiscard]] bool IsVoiceAlive(VoiceHandle handle) const;

		// ── Music ─────────────────────────────────────────────────────────
		// Streamed (MA_SOUND_FLAG_STREAM) so a multi-minute track does not sit
		// in RAM. A second call crossfades the old track out and the new one in
		// over `fadeSeconds`; StopMusic fades out and frees the slot.
		void PlayMusic(std::string_view clipPath, float fadeSeconds = 1.0f, bool loop = true, float volume = 1.0f);
		void StopMusic(float fadeSeconds = 1.0f);

		// ── Buses / settings ──────────────────────────────────────────────
		void SetBusVolume(Bus bus, float volume);
		[[nodiscard]] float GetBusVolume(Bus bus) const;
		void ApplySettings(const EngineSettings::Audio& settings); // the audio.* settings block

		// ── Frame step / listener ─────────────────────────────────────────
		// Advances crossfades and re-derives 3D gain/pan from the current
		// listener pose. Called once per game frame by AudioSystem.
		void Update(float dt, const ListenerPose& listener);
		void SetListener(const ListenerPose& listener);

		// Editor Stop/Pause. Stop ends every voice (a snapshot restore must not
		// leave a sound of the dead scene playing); Pause only silences output
		// so resume is seamless.
		void StopAllVoices();
		void SetSuspended(bool suspended);

		[[nodiscard]] Stats GetStats() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;

		void* m_engine = nullptr;             // ma_engine* (kept void*; see header note)
		void* m_context = nullptr;            // ma_context, only when we own one (headless)
		bool m_suspended = false;

		void PruneFinished();
		[[nodiscard]] bool VoiceAlive(VoiceHandle handle) const;
		[[nodiscard]] Expected<VoiceHandle> PlayInternal(std::string_view clipPath, const PlayParams& params, bool spatial, const glm::vec3& position, bool stream);
	};
} // namespace aether::audio
