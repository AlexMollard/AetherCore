#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include <glm/glm.hpp>

#include "audio/AudioMath.hpp"
#include "audio/AudioSubsystem.hpp"
#include "audio/AudioSystem.hpp"
#include "io/FileSystem.hpp"
#include "scene/AudioComponents.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether;
using namespace aether::audio;

// ── Pure math ─────────────────────────────────────────────────────────────────

TEST_CASE("attenuation model mapping validates and falls back to inverse")
{
	CHECK(AttenuationModelIndex(0) == 0);
	CHECK(AttenuationModelIndex(1) == 1);
	CHECK(AttenuationModelIndex(2) == 2);
	// Out-of-range authored values fall back to inverse, never an arbitrary model.
	CHECK(AttenuationModelIndex(-3) == 0);
	CHECK(AttenuationModelIndex(7) == 0);
}

TEST_CASE("rolloff and doppler clamps bound the spatializer parameters")
{
	CHECK(ClampRolloff(1.0f) == doctest::Approx(1.0f));
	CHECK(ClampRolloff(0.01f) == doctest::Approx(0.1f));   // below floor -> floor
	CHECK(ClampRolloff(50.0f) == doctest::Approx(10.0f));  // above ceiling -> ceiling
	CHECK(ClampDopplerFactor(1.5f) == doctest::Approx(1.5f));
	CHECK(ClampDopplerFactor(0.0f) == doctest::Approx(0.0f));  // 0 disables doppler
	CHECK(ClampDopplerFactor(-2.0f) == doctest::Approx(0.0f)); // negative would run time backwards
}

TEST_CASE("cone mapping converts degrees to radians and keeps inner <= outer")
{
	// Omni: 360/360 degrees with full outer volume.
	ConeAngles omni = MapCone(360.0f, 360.0f, 1.0f);
	CHECK(omni.innerRadians == doctest::Approx(6.2831853f).epsilon(0.0001));
	CHECK(omni.outerRadians == doctest::Approx(6.2831853f).epsilon(0.0001));
	CHECK(omni.outerVolume == doctest::Approx(1.0f));

	// A 60/120 degree cone converts to radians, inner never exceeds outer.
	ConeAngles cone = MapCone(60.0f, 120.0f, 0.2f);
	CHECK(cone.innerRadians == doctest::Approx(1.0471976f).epsilon(0.0001));
	CHECK(cone.outerRadians == doctest::Approx(2.0943951f).epsilon(0.0001));
	CHECK(cone.outerVolume == doctest::Approx(0.2f));

	// Degenerate authoring (inner > outer, negative angles, wild volume) is clamped.
	ConeAngles fixed = MapCone(200.0f, 100.0f, 5.0f);
	CHECK(fixed.outerRadians >= fixed.innerRadians);
	CHECK(fixed.innerRadians <= 6.2831853f);
	CHECK(fixed.outerVolume == doctest::Approx(1.0f));
}

TEST_CASE("effective gain chains voice*bus*master and mutes to zero")
{
	CHECK(EffectiveGain(0.8f, 0.5f, 1.0f, false) == doctest::Approx(0.4f));
	// Negative inputs are clamped, never amplified.
	CHECK(EffectiveGain(-1.0f, 0.5f, 1.0f, false) == doctest::Approx(0.0f));
	CHECK(EffectiveGain(1.0f, 1.0f, 1.0f, true) == doctest::Approx(0.0f));
}

TEST_CASE("crossfade gain is a raised cosine reaching the endpoints")
{
	// Zero-duration fades jump straight to the endpoints.
	CHECK(CrossfadeGain(0.0f, 0.0f) == doctest::Approx(0.0f));
	CHECK(CrossfadeGain(0.1f, 0.0f) == doctest::Approx(1.0f));
	// A 1 s fade: 0 at the start, 1 once complete, half-way at the midpoint.
	CHECK(CrossfadeGain(0.0f, 1.0f) == doctest::Approx(0.0f));
	CHECK(CrossfadeGain(0.5f, 1.0f) == doctest::Approx(0.5f));
	CHECK(CrossfadeGain(1.0f, 1.0f) == doctest::Approx(1.0f));
	// Clamped past the end.
	CHECK(CrossfadeGain(9.0f, 1.0f) == doctest::Approx(1.0f));
}

TEST_CASE("voice stealing picks the oldest non-music voice, never music")
{
	// Empty / all-free pools have nothing to steal.
	CHECK(PickVoiceToSteal({}) == -1);

	std::array<VoiceInfo, 4> voices{};
	voices[0] = {.birthOrder = 1, .isMusic = false, .inUse = true};
	voices[1] = {.birthOrder = 2, .isMusic = false, .inUse = true};
	voices[2] = {.birthOrder = 3, .isMusic = true, .inUse = true};
	voices[3] = {.birthOrder = 4, .isMusic = false, .inUse = true};

	// Oldest non-music wins, even though music (order 3) is older than voices 1 and 3.
	CHECK(PickVoiceToSteal(voices) == 0);

	// Music is never stolen: with only music in use, the play is refused.
	const std::array<VoiceInfo, 2> onlyMusic{VoiceInfo{.birthOrder = 1, .isMusic = true, .inUse = true}, VoiceInfo{.birthOrder = 2, .isMusic = false, .inUse = false}};
	CHECK(PickVoiceToSteal(onlyMusic) == -1);
}

// ── Component reflection round-trip ───────────────────────────────────────────

TEST_CASE("Audio Source reflection round-trips every field")
{
	const aether::reflect::ComponentType* type = aether::reflect::FindComponentType("Audio Source");
	REQUIRE(type != nullptr);

	AudioSourceComponent authored;
	authored.clipPath = "project://audio/wave.wav";
	authored.volume = 0.75f;
	authored.pitch = 1.25f;
	authored.loop = true;
	authored.spatial = false;
	authored.playOnStart = false;
	authored.minDistance = 2.0f;
	authored.maxDistance = 30.0f;
	authored.bus = static_cast<int>(Bus::Ambience);
	authored.attenuationModel = 2;
	authored.rolloff = 1.5f;
	authored.coneInnerDegrees = 60.0f;
	authored.coneOuterDegrees = 120.0f;
	authored.coneOuterVolume = 0.3f;
	authored.dopplerFactor = 2.0f;

	AudioSourceComponent loaded;
	for (const aether::reflect::FieldDesc& field: type->fields)
	{
		const aether::reflect::FieldValue value = field.get(&authored);
		field.set(&loaded, value);
	}
	CHECK(loaded.clipPath == authored.clipPath);
	CHECK(loaded.volume == doctest::Approx(authored.volume));
	CHECK(loaded.pitch == doctest::Approx(authored.pitch));
	CHECK(loaded.loop == authored.loop);
	CHECK(loaded.spatial == authored.spatial);
	CHECK(loaded.playOnStart == authored.playOnStart);
	CHECK(loaded.minDistance == doctest::Approx(authored.minDistance));
	CHECK(loaded.maxDistance == doctest::Approx(authored.maxDistance));
	CHECK(loaded.bus == authored.bus);
	CHECK(loaded.attenuationModel == authored.attenuationModel);
	CHECK(loaded.rolloff == doctest::Approx(authored.rolloff));
	CHECK(loaded.coneInnerDegrees == doctest::Approx(authored.coneInnerDegrees));
	CHECK(loaded.coneOuterDegrees == doctest::Approx(authored.coneOuterDegrees));
	CHECK(loaded.coneOuterVolume == doctest::Approx(authored.coneOuterVolume));
	CHECK(loaded.dopplerFactor == doctest::Approx(authored.dopplerFactor));

	// Enum fields read back by NAME, so a saved scene re-reads the same bus.
	const auto* busField = type->FindField("bus");
	REQUIRE(busField != nullptr);
	CHECK(busField->meta.enumTable != nullptr);
	CHECK(busField->meta.enumTable->NameOf(authored.bus) == "ambience");
}

// ── AudioSystem drive ─────────────────────────────────────────────────────────

TEST_CASE("audio system stays silent with no subsystem (UiShell profile)")
{
	ServiceContainer services;
	World world;
	AudioSystem system(services); // no AudioSubsystem registered
	const Entity e = world.Create();
	world.Emplace<AudioSourceComponent>(e, AudioSourceComponent{.clipPath = "project://audio/nothing.wav"});
	// Must not crash and never touch audio.
	system.Update(world, 0.016f);
}

// ── Headless subsystem smoke (null backend) ──────────────────────────────────

namespace
{
	// 0.25 s of 440 Hz mono 32-bit float WAV at 48 kHz - synthesized here, no
	// ISO-derived content, small enough to decode instantly.
	std::vector<std::byte> MakeTestWav()
	{
		constexpr std::uint32_t sampleRate = 48000;
		constexpr std::uint32_t seconds = 1; // 1 s so a non-looping voice is still alive during checks
		constexpr std::uint32_t sampleCount = sampleRate * seconds;
		const std::uint32_t dataBytes = sampleCount * 4;
		const std::uint32_t chunkBytes = 36 + dataBytes;

		std::vector<std::byte> bytes;
		bytes.reserve(44 + dataBytes);
		auto push = [&bytes](const void* data, std::size_t size)
		{
			const auto* p = static_cast<const std::byte*>(data);
			bytes.insert(bytes.end(), p, p + size);
		};
		auto pushU32 = [&push](std::uint32_t v)
		{
			push(&v, 4);
		};
		auto pushU16 = [&push](std::uint16_t v)
		{
			push(&v, 2);
		};

		push("RIFF", 4);
		pushU32(chunkBytes);
		push("WAVE", 4);
		push("fmt ", 4);
		pushU32(16);
		pushU16(3); // IEEE float
		pushU16(1); // mono
		pushU32(sampleRate);
		pushU32(sampleRate * 4); // byte rate
		pushU16(4);              // block align
		pushU16(32);             // bits
		push("data", 4);
		pushU32(dataBytes);
		for (std::uint32_t i = 0; i < sampleCount; ++i)
		{
			const float sample = 0.25f * std::sin(2.0f * 3.14159265358979f * 440.0f * static_cast<float>(i) / static_cast<float>(sampleRate));
			push(&sample, 4);
		}
		return bytes;
	}
} // namespace

TEST_CASE("subsystem runs headless on the null backend: plays, tracks, stops")
{
	// Scratch VFS mount: everything through io::FileSystem exactly like the game.
	const auto scratch = std::filesystem::temp_directory_path() / "aether-audio-tests";
	std::filesystem::create_directories(scratch / "audio");

	io::FileSystem::Initialize();
	io::FileSystem::Mount("project", scratch);
	const std::vector<std::byte> wav = MakeTestWav();
	const std::vector<std::byte> wavBytes = wav;
	REQUIRE(io::FileSystem::WriteFile("project://audio/beep.wav", wavBytes).has_value());

	AudioSubsystem audio;
	REQUIRE(audio.Init({.headless = true}).has_value());

	SUBCASE("volume and bus settings apply without a device")
	{
		audio.SetBusVolume(Bus::Music, 0.25f);
		CHECK(audio.GetBusVolume(Bus::Music) == doctest::Approx(0.25f));
		audio.SetListener({.position = {1.0f, 2.0f, 3.0f}, .forward = {0.0f, 0.0f, -1.0f}, .up = {0.0f, 1.0f, 0.0f}});
		audio.Update(0.016f, {});
		audio.StopAllVoices();
		audio.SetSuspended(true);
		audio.SetSuspended(false);
	}

	SUBCASE("a playable clip starts a voice the engine then tracks")
	{
		const VoiceHandle handle = audio.Play2D("project://audio/beep.wav", {.volume = 0.5f});
		CHECK(handle.IsValid());
		CHECK(audio.GetStats().startedTotal == 1);
		CHECK(audio.IsVoiceAlive(handle));

		// A missing clip fails gracefully with an invalid handle, not a crash.
		CHECK_FALSE(audio.Play2D("project://audio/does-not-exist.wav").IsValid());
		// Stopping twice, or a bogus id, is a no-op.
		audio.StopVoice(handle);
		audio.StopVoice(handle);
		audio.StopVoice(VoiceHandle{0xDEADBEEF});
	}

	SUBCASE("stop-all clears every voice including music")
	{
		CHECK(audio.Play2D("project://audio/beep.wav").IsValid());
		audio.PlayMusic("project://audio/beep.wav", 0.0f, true, 1.0f);
		audio.Update(0.016f, {});
		CHECK(audio.GetStats().musicPlaying);
		audio.StopAllVoices();
		CHECK(audio.GetStats().activeVoices == 0);
		CHECK_FALSE(audio.GetStats().musicPlaying);
	}

	SUBCASE("a play session drives play-on-start components through the system")
	{
		ServiceContainer services;
		services.Register<AudioSubsystem>(audio);
		World world;
		AudioSystem system(services);
		const Entity e = world.Create();
		world.Emplace<TransformComponent>(e);
		world.Emplace<AudioSourceComponent>(e, AudioSourceComponent{.clipPath = "project://audio/beep.wav", .loop = true, .playOnStart = true});

		const std::uint32_t before = audio.GetStats().startedTotal;
		system.Update(world, 0.016f);
		CHECK(audio.GetStats().startedTotal == before + 1);
		// Looping voice keeps its slot; a hundred more ticks must not restart it.
		for (int i = 0; i < 100; ++i)
		{
			system.Update(world, 0.016f);
		}
		CHECK(audio.GetStats().startedTotal == before + 1);
		CHECK(audio.GetStats().activeVoices == 1);
	}

	audio.Shutdown();
	io::FileSystem::Shutdown();
}
