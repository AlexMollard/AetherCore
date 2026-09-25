using System.Numerics;
namespace AetherCore;

/// <summary>
/// Game audio: fire one-shot sound effects (2D or 3D positional), stream music
/// with a crossfade, and drive the volume buses (master is a settings key; the
/// per-bus levels here are for in-game mixing like a pause menu or a mute hit).
/// Paths are VFS paths, exactly like every other asset ("project://audio/...").
/// Files may be WAV, MP3, FLAC or Ogg.
/// </summary>
public static class Audio
{
    /// <summary>Bus a voice plays on. Sfx is the default; Ambience is for looping level sound.</summary>
    public enum Bus
    {
        Music = 0,
        Sfx = 1,
        Ambience = 2,
    }

    /// <summary>Play a non-positional clip (UI clicks, global stings).</summary>
    /// <returns>Voice id for <see cref="Stop"/>, or 0 if the clip failed to load.</returns>
    public static int Play(string path, float volume = 1f, float pitch = 1f, bool loop = false, Bus bus = Bus.Sfx)
        => (int)Native.aether_audio_play_2d(path, volume, pitch, loop ? 1 : 0, (int)bus);

    /// <summary>Distance-attenuation model. Inverse is the engine default (and most engines').</summary>
    public enum AttenuationModel
    {
        Inverse = 0,
        Linear = 1,
        Exponential = 2,
    }

    /// <summary>Play a 3D positional clip at a world point through the engine's
    /// spatializer: attenuation by model/rolloff over [minDistance, maxDistance],
    /// panning, a directional cone about the emitter's facing, and doppler from
    /// relative motion (see also <c>Audio Source</c> on an entity, which feeds the
    /// emitter's velocity automatically).</summary>
    public static int PlayAt(string path, Vector3 position, float volume = 1f, float pitch = 1f, bool loop = false,
        Bus bus = Bus.Sfx, float minDistance = 1f, float maxDistance = 50f,
        AttenuationModel attenuation = AttenuationModel.Inverse, float rolloff = 1f,
        float coneInnerDegrees = 360f, float coneOuterDegrees = 360f, float coneOuterVolume = 1f, float dopplerFactor = 1f)
        => (int)Native.aether_audio_play_3d(path, position, volume, pitch, loop ? 1 : 0, (int)bus, minDistance, maxDistance,
            (int)attenuation, rolloff, coneInnerDegrees, coneOuterDegrees, coneOuterVolume, dopplerFactor);

    /// <summary>3D positional sound from an entity's current position.</summary>
    public static int PlayAt(Entity entity, string path, float volume = 1f, float pitch = 1f, bool loop = false,
        Bus bus = Bus.Sfx, float minDistance = 1f, float maxDistance = 50f,
        AttenuationModel attenuation = AttenuationModel.Inverse, float rolloff = 1f,
        float coneInnerDegrees = 360f, float coneOuterDegrees = 360f, float coneOuterVolume = 1f, float dopplerFactor = 1f)
        => PlayAt(path, Native.aether_get_position(entity.Id), volume, pitch, loop, bus, minDistance, maxDistance,
            attenuation, rolloff, coneInnerDegrees, coneOuterDegrees, coneOuterVolume, dopplerFactor);

    /// <summary>Stop a one-shot started by <see cref="Play"/>/<see cref="PlayAt"/>.</summary>
    public static void Stop(int voiceId) => Native.aether_audio_stop(voiceId);

    /// <summary>Stream a music track, crossfading (over <paramref name="fadeSeconds"/>) away
    /// from whatever is currently playing. A new call while fading keeps the handoff gapless.</summary>
    public static void PlayMusic(string path, float fadeSeconds = 1f, bool loop = true, float volume = 1f)
        => Native.aether_audio_play_music(path, fadeSeconds, loop ? 1 : 0, volume);

    /// <summary>Fade the current music out and stop it.</summary>
    public static void StopMusic(float fadeSeconds = 1f) => Native.aether_audio_stop_music(fadeSeconds);

    /// <summary>Set a bus volume in 0..1 linear gain. Master volume is the
    /// <c>audio.masterVolume</c> settings key, not this.</summary>
    public static void SetBusVolume(Bus bus, float volume) => Native.aether_audio_set_bus_volume((int)bus, volume);

    public static float GetBusVolume(Bus bus) => Native.aether_audio_get_bus_volume((int)bus);

    /// <summary>Start the Audio Source component on this entity playing now
    /// (same path as its playOnStart flag).</summary>
    public static void PlaySource(Entity entity) => Native.aether_audio_source_play(entity.Id);
}
