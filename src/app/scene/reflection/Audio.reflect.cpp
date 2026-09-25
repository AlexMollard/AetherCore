// Audio Source reflection: one declaration drives the Add-Component palette,
// the inspector drawer, MCP get/set_component and the scene-serializer TOML
// round-trip. Data-only component, so serialization is the generic codec.

#include "scene/reflection/Reflection.hpp"

#include "Icons.hpp"

#include "audio/AudioMath.hpp"
#include "scene/AudioComponents.hpp"

namespace
{
	const aether::reflect::EnumTable& AudioBusEnum()
	{
		static const aether::reflect::EnumTable table{{
		        {"music", static_cast<int>(aether::audio::Bus::Music)},
		        {"sfx", static_cast<int>(aether::audio::Bus::Sfx)},
		        {"ambience", static_cast<int>(aether::audio::Bus::Ambience)},
		}};
		return table;
	}
} // namespace

AE_COMPONENT(AudioSourceComponent, "Audio Source", "Audio", ICON_FA_VOLUME_HIGH)
AE_FIELD_T(clipPath, String, "VFS path to the clip (project:// or engine://). WAV, MP3, FLAC or Ogg. Empty = silent.")
AE_FIELD_RT(volume, Float, 0.0f, 1.0f, "Voice volume, on top of the bus and master volumes.")
AE_FIELD_RT(pitch, Float, 0.1f, 4.0f, "Playback rate. 1 is normal.")
AE_FIELD_T(loop, Bool, "Restart from the beginning when the clip ends.")
AE_FIELD_T(spatial, Bool, "Positional: volume and pan follow this entity relative to the main camera. Off plays at full level (UI, global stings).")
AE_FIELD_T(playOnStart, Bool, "Start playing as soon as the play session begins.")
AE_FIELD_RT(minDistance, Float, 0.0f, 100.0f, "Full volume inside this distance (spatial only).")
AE_FIELD_RT(maxDistance, Float, 0.0f, 500.0f, "Distance at which the attenuation curve clamps (spatial only).")
AE_FIELD_ENUM("bus", bus, AudioBusEnum())
AE_FIELD_NT("attenuation_model", attenuationModel, Int, "Distance curve: 0 = inverse (default), 1 = linear, 2 = exponential. Spatial only; anything else falls back to inverse.")
AE_FIELD_RT(rolloff, Float, 0.1f, 10.0f, "Attenuation curve intensity. 1 is the physically-plausible default. Spatial only.")
AE_FIELD_NT("cone_inner_degrees", coneInnerDegrees, Float, "Directional cone about the emitter's facing: full volume inside this opening angle. 360 = omni-directional. Spatial only.")
AE_FIELD_NT("cone_outer_degrees", coneOuterDegrees, Float, "Outside this opening angle the cone's outer volume applies; between the angles the gain fades. Spatial only.")
AE_FIELD_RT(coneOuterVolume, Float, 0.0f, 1.0f, "Gain outside the cone (0 = silent behind the emitter). Spatial only.")
AE_FIELD_RT(dopplerFactor, Float, 0.0f, 5.0f, "Doppler effect strength from relative motion. 0 disables it. Spatial only.")
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
