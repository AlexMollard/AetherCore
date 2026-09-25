// Managed Audio surface. Runtime-safe: engine headers only, no editor catalogs,
// no ImGui - mirrors Physics2DExports.cpp conventions (blittable ABI, silent
// no-ops when audio is unavailable, UTF-8 strings via the shared marshalling).

#include "scripting/interop/InteropCommon.hpp"

#include <cstdint>
#include <string>

#include "audio/AudioSubsystem.hpp"
#include "scene/AudioComponents.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	aether::audio::AudioSubsystem* Audio()
	{
		return ActiveContext().audio;
	}

	aether::audio::PlayParams Params(float volume, float pitch, std::int32_t loop, std::int32_t bus, float minDistance, float maxDistance)
	{
		return aether::audio::PlayParams{
		        .volume = volume,
		        .pitch = pitch,
		        .loop = loop != 0,
		        .bus = static_cast<aether::audio::Bus>(bus),
		        .minDistance = minDistance,
		        .maxDistance = maxDistance,
		};
	}

	aether::audio::PlayParams SpatialParams(float volume, float pitch, std::int32_t loop, std::int32_t bus, float minDistance, float maxDistance, std::int32_t attenuationModel, float rolloff, float coneInnerDegrees, float coneOuterDegrees, float coneOuterVolume, float dopplerFactor)
	{
		auto params = Params(volume, pitch, loop, bus, minDistance, maxDistance);
		params.attenuationModel = attenuationModel;
		params.rolloff = rolloff;
		params.coneInnerDegrees = coneInnerDegrees;
		params.coneOuterDegrees = coneOuterDegrees;
		params.coneOuterVolume = coneOuterVolume;
		params.dopplerFactor = dopplerFactor;
		return params;
	}
} // namespace

// ── One-shots ─────────────────────────────────────────────────────────────────

AE_SCRIPT_API std::int32_t aether_audio_play_2d(const char* clipPath, float volume, float pitch, std::int32_t loop, std::int32_t bus)
{
	return SafeExport([&] -> std::int32_t
	{
		if (auto* audio = Audio(); audio != nullptr && clipPath != nullptr)
		{
			const aether::audio::VoiceHandle handle = audio->Play2D(clipPath, Params(volume, pitch, loop, bus, 1.0f, 50.0f));
			return static_cast<std::int32_t>(handle.id);
		}
		return 0;
	});
}

AE_SCRIPT_API std::int32_t aether_audio_play_3d(const char* clipPath, Vec3 position, float volume, float pitch, std::int32_t loop, std::int32_t bus, float minDistance, float maxDistance, std::int32_t attenuationModel, float rolloff, float coneInnerDegrees, float coneOuterDegrees, float coneOuterVolume, float dopplerFactor)
{
	return SafeExport([&] -> std::int32_t
	{
		if (auto* audio = Audio(); audio != nullptr && clipPath != nullptr)
		{
			const aether::audio::VoiceHandle handle = audio->Play3D(clipPath, ToGlm(position), SpatialParams(volume, pitch, loop, bus, minDistance, maxDistance, attenuationModel, rolloff, coneInnerDegrees, coneOuterDegrees, coneOuterVolume, dopplerFactor));
			return static_cast<std::int32_t>(handle.id);
		}
		return 0;
	});
}

AE_SCRIPT_API void aether_audio_stop(std::int32_t voiceId)
{
	SafeExport([&] -> void
	{
		if (auto* audio = Audio(); audio != nullptr && voiceId != 0)
		{
			audio->StopVoice(aether::audio::VoiceHandle{static_cast<std::uint32_t>(voiceId)});
		}
	});
}

// ── Music ─────────────────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_audio_play_music(const char* clipPath, float fadeSeconds, std::int32_t loop, float volume)
{
	SafeExport([&] -> void
	{
		if (auto* audio = Audio(); audio != nullptr && clipPath != nullptr)
		{
			audio->PlayMusic(clipPath, fadeSeconds, loop != 0, volume);
		}
	});
}

AE_SCRIPT_API void aether_audio_stop_music(float fadeSeconds)
{
	SafeExport([&] -> void
	{
		if (auto* audio = Audio())
		{
			audio->StopMusic(fadeSeconds);
		}
	});
}

// ── Buses ─────────────────────────────────────────────────────────────────────

AE_SCRIPT_API void aether_audio_set_bus_volume(std::int32_t bus, float volume)
{
	SafeExport([&] -> void
	{
		if (auto* audio = Audio())
		{
			audio->SetBusVolume(static_cast<aether::audio::Bus>(bus), volume);
		}
	});
}

AE_SCRIPT_API float aether_audio_get_bus_volume(std::int32_t bus)
{
	return SafeExport([&] -> float
	{
		if (auto* audio = Audio())
		{
			return audio->GetBusVolume(static_cast<aether::audio::Bus>(bus));
		}
		return 0.0f;
	});
}

// ── AudioSource component control ─────────────────────────────────────────────
namespace
{
	void ForEachSource(std::uint32_t id, auto&& fn)
	{
		auto& world = ActiveWorld();
		const aether::Entity entity{id};
		if (auto* source = world.TryGet<aether::AudioSourceComponent>(entity))
		{
			fn(*source);
		}
	}
} // namespace

AE_SCRIPT_API void aether_audio_source_play(std::uint32_t id)
{
	SafeExport([&] -> void
	{
		// "Play now" is just the play-on-start path run once: the system picks
		// it up on its next tick, so all lifetime/position bookkeeping stays in
		// one place.
		ForEachSource(id, [](aether::AudioSourceComponent& source)
		{
			source.playOnStart = true;
		});
	});
}
