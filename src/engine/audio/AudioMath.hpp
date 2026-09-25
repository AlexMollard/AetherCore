#pragma once

// Pure, allocation-free audio math. Deliberately free of miniaudio so the
// attenuation curve, stereo panning, crossfade and voice-stealing policy are
// unit-testable headless (tests/audio/AudioTests.cpp) and can never drift
// between the null-backend tests and a real device.

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace aether::audio
{
	// Buses a voice can play on. Each has its own settings volume; a voice's
	// final gain is master * bus * voice volume. Order is the interop ABI.
	enum class Bus : std::int32_t
	{
		Music = 0,
		Sfx = 1,
		Ambience = 2,
	};

	// Linear rolloff: full volume up to minDistance, silent at maxDistance and
	// beyond. A min >= max edge (an authored degenerate range) means no
	// attenuation at all rather than a divide by zero.
	[[nodiscard]] inline float LinearAttenuation(float distance, float minDistance, float maxDistance)
	{
		if (minDistance >= maxDistance)
		{
			return 1.0f;
		}
		const float d = glm::max(distance, minDistance);
		return glm::clamp(1.0f - (d - minDistance) / (maxDistance - minDistance), 0.0f, 1.0f);
	}

	// ── Parameter mapping into miniaudio ─────────────────────────────────────
	// miniaudio owns the actual spatialisation (attenuation, panning, cone,
	// doppler); these pure helpers are the validated boundary between authored
	// component values and what we hand to it.

	// Attenuation model as authored (audio_source TOML/interop): 0 inverse
	// (the engine default), 1 linear, 2 exponential. Anything else falls back
	// to inverse rather than picking an arbitrary model.
	[[nodiscard]] inline int AttenuationModelIndex(int authored)
	{
		return (authored >= 0 && authored <= 2) ? authored : 0;
	}

	// Rolloff intensity. 1 is the physically-plausible default; 0 would
	// silence the model entirely, negative is nonsense.
	[[nodiscard]] inline float ClampRolloff(float rolloff)
	{
		return std::clamp(rolloff, 0.1f, 10.0f);
	}

	// Doppler effect strength. 0 disables the effect for the voice; negative
	// would run time backwards.
	[[nodiscard]] inline float ClampDopplerFactor(float factor)
	{
		return std::max(factor, 0.0f);
	}

	// Cone as miniaudio wants it: inner/outer half? no - FULL opening angles
	// in radians, inner <= outer, plus the gain outside the cone. An outer
	// angle >= 360 means omni-directional (no cone).
	struct ConeAngles
	{
		float innerRadians = 6.2831853f;
		float outerRadians = 6.2831853f;
		float outerVolume = 1.0f;
	};

	[[nodiscard]] inline ConeAngles MapCone(float innerDegrees, float outerDegrees, float outerVolume)
	{
		constexpr float kPi = 3.14159265358979323846f;
		const float inner = std::clamp(innerDegrees, 0.0f, 360.0f);
		const float outer = std::clamp(std::max(outerDegrees, inner), 0.0f, 360.0f);
		ConeAngles cone;
		cone.innerRadians = inner * kPi / 180.0f;
		cone.outerRadians = outer * kPi / 180.0f;
		cone.outerVolume = std::clamp(outerVolume, 0.0f, 1.0f);
		return cone;
	}

	// The whole gain chain in one place: voice volume, then bus, then master,
	// then the global mute. Keeping it as one function is what makes "mute"
	// and the volume sliders behave identically for 2D, 3D and music.
	[[nodiscard]] inline float EffectiveGain(float voiceVolume, float busVolume, float masterVolume, bool muted)
	{
		if (muted)
		{
			return 0.0f;
		}
		return glm::max(voiceVolume, 0.0f) * glm::max(busVolume, 0.0f) * glm::max(masterVolume, 0.0f);
	}

	// Fade curve for PlayMusic crossfades: 0 at fade start, 1 once the fade
	// completes. A raised-cosine rather than a straight line, so a loop point
	// or a skipped track does not click.
	[[nodiscard]] inline float CrossfadeGain(float elapsed, float fadeSeconds)
	{
		if (fadeSeconds <= 0.0f)
		{
			return elapsed > 0.0f ? 1.0f : 0.0f;
		}
		const float t = glm::clamp(elapsed / fadeSeconds, 0.0f, 1.0f);
		return 0.5f - 0.5f * glm::cos(t * 3.14159265358979323846f);
	}

	// Minimal per-voice facts the stealing policy sees. `birthOrder` is a
	// monotonically increasing counter, `isMusic` protects the one voice a
	// level cannot afford to lose.
	struct VoiceInfo
	{
		std::uint64_t birthOrder = 0;
		bool isMusic = false;
		bool inUse = false;
	};

	// Index of the voice to reclaim when the pool is full: the OLDEST
	// non-music voice. Music is never stolen (StopMusic or a new crossfade is
	// the only thing that ends it); with no music playing that is the
	// first-in voice, the classic oldest-first policy, and looping one-shots
	// that have been playing longest are exactly the ones nobody is waiting on.
	// Returns -1 when only music remains (the caller then refuses the play).
	[[nodiscard]] inline std::int32_t PickVoiceToSteal(std::span<const VoiceInfo> voices)
	{
		std::int32_t victim = -1;
		std::uint64_t oldest = ~0ull;
		for (std::int32_t i = 0; i < static_cast<std::int32_t>(voices.size()); ++i)
		{
			const VoiceInfo& v = voices[i];
			if (!v.inUse || v.isMusic)
			{
				continue;
			}
			if (v.birthOrder < oldest)
			{
				oldest = v.birthOrder;
				victim = i;
			}
		}
		return victim;
	}
} // namespace aether::audio
