# Audio

The engine's audio stack: a miniaudio-backed subsystem in the engine lifecycle,
a reflected `Audio Source` component, a C# `Audio.*` scripting API, and the
volume buses wired into the settings cascade.

## Backend

[miniaudio](https://github.com/mackron/miniaudio) (0.11.x, via CPM, public
domain). One header; the `MINIAUDIO_IMPLEMENTATION` TU is
`src/engine/audio/AudioSubsystem.cpp` alone. WASAPI on Windows, ALSA on Linux,
plus a **null backend** used by tests, CI and machines with no audio device -
the engine always initialises, it just stays silent. Decoding covers WAV, MP3,
FLAC and Ogg-Vorbis with no extra dependency.

Clips are loaded through `io::FileSystem` via a custom `ma_vfs`, so
`project://`, `engine://` and paks all work. The subsystem is initialised only
on the Full runtime profile - the Launcher's UiShell brings up no audio at all.

## Lifecycle

- **Init**: last in the `AetherCore` constructor (Full profile only), after the
  render passes; it needs nothing but the VFS mounts made at the top of the
  constructor.
- **Shutdown**: first in `~AetherCore`, before the render thread is even
  joined - its device thread mixes into clip buffers whose VFS paths must
  outlive it. Teardown order inside: sounds → bus groups → engine → resource
  manager → context.

## Buses and volumes

Buses: `Music`, `Sfx`, `Ambience`, plus the master gain. A voice's final gain
is `voice volume × bus × master`, and `audio.muted` zeroes the endpoint
instantly. All of it lives in the settings cascade (`audio.masterVolume`,
`audio.musicVolume`, `audio.sfxVolume`, `audio.ambienceVolume`,
`audio.muted`) - reflected in `EngineSettings`, editable in the settings UI,
saved per the usual user/project split.

## 3D / spatial audio

Spatialisation is **miniaudio's own spatializer** - attenuation, panning,
cones and doppler all happen in the mixer; the engine's job is to feed it:

- The **listener** is the main camera, published once per game frame by
  `AudioSystem` (game thread only, after `CameraSystem` has published the
  camera the view is drawn with): position, direction, world-up and the
  camera's **velocity** (position delta per frame, driving doppler).
- Each spatial voice carries its entity's world **position and velocity**
  (also position delta), plus its authored parameters:
  - `attenuation_model`: inverse (default, like most engines), linear or
    exponential;
  - `rolloff`: curve intensity (default 1);
  - `min_distance` / `max_distance`: attenuation clamp range;
  - `cone_inner_degrees` / `cone_outer_degrees` / `cone_outer_volume`:
    directional cone about the emitter's facing (>= 360 = omni);
  - `doppler_factor`: doppler strength (0 disables).
- The boundary between authored values and the miniaudio API is pure, unit
  tested code (`audio::AudioMath`: `AttenuationModelIndex`, `ClampRolloff`,
  `ClampDopplerFactor`, `MapCone`). Dev builds log a one-line spatial stats
  sample every ~120 frames per emitter (relative position, distance, velocity);
  ship builds log nothing.

## Playback paths

- **One-shots** (`Audio.Play`, `Audio.PlayAt`, or an `Audio Source`
  component): decoded fully at start (`MA_SOUND_FLAG_DECODE`) - SFX are small
  and must never stall on disk. A 64-voice pool; when full, the **oldest
  non-music voice is stolen** (`audio::PickVoiceToSteal`, tested).
- **Music** (`Audio.PlayMusic`): streamed (`MA_SOUND_FLAG_STREAM`) and
  crossfaded - a new track fades the old one out over the same window, and a
  failed load leaves the current track playing. Music voices are never stolen;
  `StopMusic`/`StopAllVoices` are the only things that end them.

## Audio Source component

Reflected (`src/app/scene/reflection/Audio.reflect.cpp`), so the scene TOML
round-trip, inspector, Add-Component palette and MCP `get/set_component` all
work with zero bespoke code: `clipPath`, `volume`, `pitch`, `loop`, `spatial`,
`playOnStart`, `minDistance`, `maxDistance`, `bus`. `playOnStart` fires the
first frame of a play session; finished voices free their slot, so a
re-triggered component restarts cleanly.

## Scripting API (C#)

```csharp
int v = Audio.Play("project://audio/crate.wav");                  // 2D
Audio.PlayAt(entity, "project://audio/water.wav");                // 3D at entity
Audio.PlayMusic("project://music/beach.ogg", fadeSeconds: 1.5f);  // streamed
Audio.StopMusic(2f);
Audio.SetBusVolume(Audio.Bus.Sfx, 0.8f);
Audio.PlaySource(entity);          // start an Audio Source component now
Audio.Stop(v);
```

Interop exports live in `src/app/scripting/interop/AudioExports.cpp`
(runtime-safe: no editor dependencies) as `aether_audio_*`; P/Invoke
declarations in `managed/AetherCore/Internal/Native.cs`, public API in
`managed/AetherCore/Audio.cs`.

## Play / pause / stop

Because `AudioSystem` is a World system, edit mode never starts audio. The
editor's Play controls additionally drive the subsystem directly
(`src/app/PlaySession.cpp`):

- **Stop**: `StopAllVoices()` - every one-shot *and* the level music dies with
  the play session, before the scene snapshot is restored.
- **Pause/Resume/Step**: `SetSuspended(true/false)` stops the mix device, so
  pause is a hard silence and resume is seamless.

The published game runs the same subsystem through the same systems; a machine
with no audio device silently falls back to the null backend.

## Tests

`tests/audio/AudioTests.cpp` (doctest, headless): spatializer parameter
mapping, gain chain, crossfade curve, voice-stealing policy, the reflected component
round-trip, the "no audio subsystem" UiShell case, and a null-backend smoke
that plays a synthesized WAV through the real VFS path and tracks voices.
