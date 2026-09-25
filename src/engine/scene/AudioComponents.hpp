#pragma once

#include <string>

#include <glm/glm.hpp>

namespace aether
{
	// A sound an entity emits. Authored entirely through the reflected fields
	// (scene TOML, inspector, MCP); the audio::AudioSystem drives it each frame
	// on the game thread. Runtime state (which voice is playing) lives in the
	// system-side map keyed by entity, NOT here - this stays pure data so the
	// serializer round-trips it unchanged.
	struct AudioSourceComponent
	{
		// VFS path to the clip (project://, engine://). Empty = silent.
		std::string clipPath;
		float volume = 1.0f;
		float pitch = 1.0f;
		bool loop = false;
		// 2D sounds play at full level regardless of listener position.
		bool spatial = true;
		bool playOnStart = true;
		// Linear attenuation range in world units (spatial only).
		float minDistance = 1.0f;
		float maxDistance = 50.0f;
		// Sfx by default; ambience for looping level sound, music for a level's own track.
		int bus = 1; // audio::Bus::Sfx

		// ── Spatialiser (miniaudio) parameters, spatial only ─────────────
		// Attenuation with distance: 0 inverse (default, like most engines),
		// 1 linear, 2 exponential.
		int attenuationModel = 0;
		// Intensity of the attenuation curve; 1 is the physically-plausible default.
		float rolloff = 1.0f;
		// Directional cone about the emitter's facing (its forward = -Z of its
		// world rotation). >= 360 degrees = omni-directional.
		float coneInnerDegrees = 360.0f;
		float coneOuterDegrees = 360.0f;
		// Gain outside the cone (between the angles it fades 1 -> outerVolume).
		float coneOuterVolume = 1.0f;
		// Doppler effect strength from emitter/listener relative velocity; 0 disables.
		float dopplerFactor = 1.0f;
	};
} // namespace aether
