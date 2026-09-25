#include "audio/AudioSubsystem.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <glm/geometric.hpp>

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "io/FileSystem.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::audio
{
	namespace
	{
		constexpr std::size_t kMaxVoices = 64;

		Bus ClampBus(std::int32_t bus)
		{
			if (bus < static_cast<std::int32_t>(Bus::Music) || bus > static_cast<std::int32_t>(Bus::Ambience))
			{
				return Bus::Sfx;
			}
			return static_cast<Bus>(bus);
		}

		// ── VFS bridge ─────────────────────────────────────────────────────
		// miniaudio's resource manager asks a ma_vfs for bytes; we answer from
		// io::FileSystem so clips load through project:// / engine:// and paks
		// exactly like every other asset. Read-only: a write request is refused.
		struct VfsFile
		{
			std::vector<std::byte> bytes;
			ma_uint64 cursor = 0;
		};

		ma_result VfsOpen(ma_vfs* vfs, const char* path, ma_uint32 openMode, ma_vfs_file* file)
		{
			if ((openMode & MA_OPEN_MODE_READ) == 0 || path == nullptr || file == nullptr)
			{
				return MA_ACCESS_DENIED;
			}
			auto bytes = io::FileSystem::ReadFile(path);
			if (!bytes.has_value())
			{
				AE_WARN(LogCategory::Audio, "Audio clip not readable: {} ({})", path, bytes.error().ToString());
				return MA_DOES_NOT_EXIST;
			}
			auto* handle = new VfsFile{std::move(*bytes), 0};
			*file = reinterpret_cast<ma_vfs_file>(handle);
			return MA_SUCCESS;
		}

		ma_result VfsOpenW([[maybe_unused]] ma_vfs* vfs, [[maybe_unused]] const wchar_t* path, [[maybe_unused]] ma_uint32 openMode, [[maybe_unused]] ma_vfs_file* file) { return MA_ACCESS_DENIED; }

		ma_result VfsClose(ma_vfs* vfs, ma_vfs_file file)
		{
			delete reinterpret_cast<VfsFile*>(file);
			return MA_SUCCESS;
		}

		ma_result VfsRead(ma_vfs* vfs, ma_vfs_file file, void* destination, size_t bytesToRead, size_t* bytesRead)
		{
			auto* f = reinterpret_cast<VfsFile*>(file);
			const ma_uint64 remaining = f->bytes.size() - std::min<ma_uint64>(f->cursor, f->bytes.size());
			const auto toRead = static_cast<std::size_t>(std::min<ma_uint64>(bytesToRead, remaining));
			std::memcpy(destination, f->bytes.data() + f->cursor, toRead);
			f->cursor += toRead;
			if (bytesRead != nullptr)
			{
				*bytesRead = toRead;
			}
			return MA_SUCCESS;
		}

		ma_result VfsWrite([[maybe_unused]] ma_vfs* vfs, [[maybe_unused]] ma_vfs_file file, [[maybe_unused]] const void* source, [[maybe_unused]] size_t bytesToWrite, [[maybe_unused]] size_t* bytesWritten) { return MA_ACCESS_DENIED; }

		ma_result VfsSeek(ma_vfs* vfs, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
		{
			auto* f = reinterpret_cast<VfsFile*>(file);
			ma_int64 target = f->cursor;
			if (origin == ma_seek_origin_start)
			{
				target = offset;
			}
			else if (origin == ma_seek_origin_current)
			{
				target += offset;
			}
			else
			{
				target = static_cast<ma_int64>(f->bytes.size()) + offset;
			}
			f->cursor = static_cast<ma_uint64>(std::clamp<ma_int64>(target, 0, static_cast<ma_int64>(f->bytes.size())));
			return MA_SUCCESS;
		}

		ma_result VfsTell(ma_vfs* vfs, ma_vfs_file file, ma_int64* cursor)
		{
			if (cursor != nullptr)
			{
				*cursor = static_cast<ma_int64>(reinterpret_cast<VfsFile*>(file)->cursor);
			}
			return MA_SUCCESS;
		}

		ma_result VfsInfo(ma_vfs* vfs, ma_vfs_file file, ma_file_info* info)
		{
			if (info != nullptr)
			{
				info->sizeInBytes = reinterpret_cast<VfsFile*>(file)->bytes.size();
			}
			return MA_SUCCESS;
		}

		// ma_vfs is `typedef void`: the resource manager takes an opaque pointer it
		// dispatches back through ma_vfs_callbacks, so the callbacks object doubles
		// as the handle.
		ma_vfs_callbacks g_vfsCallbacks{
		        .onOpen = VfsOpen,
		        .onOpenW = VfsOpenW,
		        .onClose = VfsClose,
		        .onRead = VfsRead,
		        .onWrite = VfsWrite,
		        .onSeek = VfsSeek,
		        .onTell = VfsTell,
		        .onInfo = VfsInfo,
		};
	} // namespace

	// Everything miniaudio owns, in one place with a deterministic teardown
	// order: sounds die before the groups they attach to, groups before the
	// resource manager, resource manager before the engine.
	struct AudioSubsystem::Impl
	{
		ma_resource_manager* resourceManager = nullptr;
		ma_engine* engine = nullptr;
		ma_sound_group* groups[3] = {nullptr, nullptr, nullptr}; // indexed by Bus

		struct Voice
		{
			ma_sound sound{};
			bool inUse = false;
			bool spatial = false;
			glm::vec3 position{0.0f};
			glm::vec3 velocity{0.0f};
			float voiceVolume = 1.0f;
			Bus bus = Bus::Sfx;
			std::uint64_t birthOrder = 0;
			std::uint32_t generation = 0; // recycled id guard
		};

		Voice voices[kMaxVoices]{};
		std::uint32_t nextId = 1;
		std::uint64_t nextBirth = 1;

		float busVolumes[3] = {1.0f, 1.0f, 1.0f};
		float masterVolume = 1.0f;
		bool muted = false;

		// Music: one streaming voice plus a crossfade. The outgoing track keeps
		// playing (faded) until its fade finishes, so the handoff never gaps.
		struct MusicTrack
		{
			ma_sound* sound = nullptr;
			bool fadingOut = false;
			float fadeElapsed = 0.0f;
			float fadeSeconds = 0.0f;
			float volume = 1.0f;
			std::string path;
		};
		MusicTrack current{};
		MusicTrack outgoing{};

		ListenerPose listener{};
		std::uint32_t startedTotal = 0;
	};

	AudioSubsystem::AudioSubsystem() = default;

	AudioSubsystem::~AudioSubsystem()
	{
		Shutdown();
	}

	Expected<void> AudioSubsystem::Init(const Config& config)
	{
		if (m_engine != nullptr)
		{
			return std::unexpected(AetherError{.category = LogCategory::Audio, .message = "AudioSubsystem already initialized"});
		}
		m_impl = std::make_unique<Impl>();

		auto* resourceManager = new ma_resource_manager{};
		ma_resource_manager_config resourceConfig = ma_resource_manager_config_init();
		resourceConfig.pVFS = reinterpret_cast<ma_vfs*>(&g_vfsCallbacks);
		ma_result result = ma_resource_manager_init(&resourceConfig, resourceManager);
		if (result != MA_SUCCESS)
		{
			delete resourceManager;
			m_impl.reset();
			return std::unexpected(AetherError{.category = LogCategory::Audio, .message = std::string{"Failed to init audio resource manager: "} + ma_result_description(result)});
		}
		m_impl->resourceManager = resourceManager;

		auto* engine = new ma_engine{};
		ma_engine_config engineConfig = ma_engine_config_init();
		engineConfig.pResourceManager = resourceManager;

		// Headless (tests, CI, no-audio machines): restrict the context to the
		// null backend, which always opens - the full mixing pipeline runs, the
		// OS device does not. The context must outlive the engine, so it is
		// owned here and freed in Shutdown.
		ma_context* context = nullptr;
		if (config.headless)
		{
			context = new ma_context{};
			constexpr ma_backend kNullBackend[1] = {ma_backend_null};
			if (ma_context_init(kNullBackend, 1, nullptr, context) != MA_SUCCESS)
			{
				delete context;
				context = nullptr;
			}
			engineConfig.pContext = context;
		}

		const ma_result engineResult = ma_engine_init(&engineConfig, engine);
		if (engineResult != MA_SUCCESS)
		{
			if (context != nullptr)
			{
				ma_context_uninit(context);
				delete context;
			}
			ma_resource_manager_uninit(resourceManager);
			delete resourceManager;
			delete engine;
			m_impl.reset();
			return std::unexpected(AetherError{.category = LogCategory::Audio, .message = std::string{"Failed to init audio engine: "} + ma_result_description(engineResult)});
		}
		m_impl->engine = engine;
		m_engine = engine;
		m_context = context;

		for (const Bus bus : {Bus::Music, Bus::Sfx, Bus::Ambience})
		{
			auto* group = new ma_sound_group{};
			// NULL parent = the engine endpoint, the mix root.
			const ma_result groupResult = ma_sound_group_init(engine, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, group);
			if (groupResult != MA_SUCCESS)
			{
				AE_ERROR(LogCategory::Audio, "Failed to init sound group {}: {}", static_cast<int>(bus), ma_result_description(groupResult));
				delete group;
				Shutdown();
				return std::unexpected(AetherError{.category = LogCategory::Audio, .message = "Failed to init audio bus group"});
			}
			m_impl->groups[static_cast<int>(bus)] = group;
		}

		AE_INFO(LogCategory::Audio, "Audio initialized ({} backend, {} voice pool).", config.headless ? "null" : "default", kMaxVoices);
		return {};
	}

	void AudioSubsystem::Shutdown()
	{
		if (m_impl == nullptr)
		{
			return;
		}

		// Order matters: stop and free every sound while the engine still
		// exists, then groups, then the engine, then the resource manager whose
		// decoders the sounds were using. Init failure paths may leave any of
		// these null, so each step re-checks.
		if (m_impl->engine != nullptr)
		{
			StopAllVoices();
			for (ma_sound_group* group: m_impl->groups)
			{
				if (group != nullptr)
				{
					ma_sound_group_uninit(group);
					delete group;
				}
			}
			ma_engine_uninit(m_impl->engine);
			delete m_impl->engine;
			m_impl->engine = nullptr;
		}
		if (m_impl->resourceManager != nullptr)
		{
			ma_resource_manager_uninit(m_impl->resourceManager);
			delete m_impl->resourceManager;
			m_impl->resourceManager = nullptr;
		}
		m_impl.reset();
		if (m_context != nullptr)
		{
			ma_context_uninit(static_cast<ma_context*>(m_context));
			delete static_cast<ma_context*>(m_context);
			m_context = nullptr;
		}
		m_engine = nullptr;
	}

	Expected<VoiceHandle> AudioSubsystem::PlayInternal(std::string_view clipPath, const PlayParams& params, bool spatial, const glm::vec3& position, bool stream)
	{
		if (m_impl == nullptr || m_impl->engine == nullptr)
		{
			return std::unexpected(AetherError{.category = LogCategory::Audio, .message = "Audio subsystem not initialized"});
		}
		Impl& impl = *m_impl;
		ma_sound_group* group = impl.groups[static_cast<int>(params.bus)];
		if (group == nullptr)
		{
			return std::unexpected(AetherError{.category = LogCategory::Audio, .message = "Audio bus not initialized"});
		}

		// Voice pool. Steal per the tested policy before refusing outright.
		std::int32_t slot = -1;
		for (std::int32_t i = 0; i < static_cast<std::int32_t>(kMaxVoices); ++i)
		{
			if (!impl.voices[i].inUse)
			{
				slot = i;
				break;
			}
		}
		if (slot < 0)
		{
			std::array<VoiceInfo, kMaxVoices> voiceInfos{};
			for (std::size_t i = 0; i < kMaxVoices; ++i)
			{
				voiceInfos[i] = VoiceInfo{.birthOrder = impl.voices[i].birthOrder, .isMusic = false, .inUse = impl.voices[i].inUse};
			}
			const std::int32_t victim = PickVoiceToSteal(voiceInfos);
			if (victim < 0)
			{
				AE_WARN(LogCategory::Audio, "Voice pool exhausted by music; refusing play: {}", clipPath);
				return std::unexpected(AetherError{.category = LogCategory::Audio, .message = "Voice pool exhausted"});
			}
			slot = victim;
			ma_sound_uninit(&impl.voices[slot].sound);
			AE_WARN(LogCategory::Audio, "Stole oldest voice for: {}", clipPath);
		}

		Impl::Voice& voice = impl.voices[slot];
		const std::uint32_t generation = ++voice.generation;

		std::string path(clipPath);
		// Spatial sounds use miniaudio's spatializer (attenuation, panning,
		// cone, doppler); 2D and music opt out of it.
		const ma_uint32 spatialFlag = spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
		const ma_uint32 loadFlag = stream ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
		const ma_uint32 flags = loadFlag | spatialFlag;
		const ma_result result = ma_sound_init_from_file(m_impl->engine, path.c_str(), flags, group, nullptr, &voice.sound);
		if (result != MA_SUCCESS)
		{
			AE_WARN(LogCategory::Audio, "Failed to open clip {} ({})", path, ma_result_description(result));
			return std::unexpected(AetherError{.category = LogCategory::Audio, .message = std::string{"Failed to open audio clip: "} + ma_result_description(result)});
		}

		voice.inUse = true;
		voice.spatial = spatial;
		voice.position = position;
		voice.velocity = glm::vec3{0.0f};
		voice.voiceVolume = params.volume;
		voice.bus = params.bus;
		voice.birthOrder = impl.nextBirth++;

		ma_sound_set_looping(&voice.sound, params.loop ? MA_TRUE : MA_FALSE);
		ma_sound_set_pitch(&voice.sound, glm::max(params.pitch, 0.01f));

		if (spatial)
		{
			// Hand the sound to miniaudio's spatializer: attenuation model,
			// rolloff, distances, cone and doppler, plus the world position.
			// Velocity follows once AudioSystem starts reporting movement.
			static constexpr ma_attenuation_model kModels[3] = {
			        ma_attenuation_model_inverse,
			        ma_attenuation_model_linear,
			        ma_attenuation_model_exponential,
			};
			ma_sound_set_position(&voice.sound, position.x, position.y, position.z);
			ma_sound_set_velocity(&voice.sound, 0.0f, 0.0f, 0.0f);
			ma_sound_set_attenuation_model(&voice.sound, kModels[AttenuationModelIndex(params.attenuationModel)]);
			ma_sound_set_rolloff(&voice.sound, ClampRolloff(params.rolloff));
			ma_sound_set_min_distance(&voice.sound, glm::max(params.minDistance, 0.0f));
			ma_sound_set_max_distance(&voice.sound, glm::max(params.maxDistance, 0.0f));
			const ConeAngles cone = MapCone(params.coneInnerDegrees, params.coneOuterDegrees, params.coneOuterVolume);
			ma_sound_set_cone(&voice.sound, cone.innerRadians, cone.outerRadians, cone.outerVolume);
			ma_sound_set_doppler_factor(&voice.sound, ClampDopplerFactor(params.dopplerFactor));
		}
		ma_sound_set_volume(&voice.sound, 1.0f);
		ma_sound_start(&voice.sound);

		++impl.startedTotal;
		return VoiceHandle{(generation << 16) | static_cast<std::uint32_t>(slot) | (1u << 31)};
	}

	VoiceHandle AudioSubsystem::Play2D(std::string_view clipPath, const PlayParams& params)
	{
		const Expected<VoiceHandle> handle = PlayInternal(clipPath, params, false, {}, false);
		if (handle.has_value())
		{
			AE_INFO(LogCategory::Audio, "Play2D {} -> voice {} ({} active).", clipPath, handle->id, GetStats().activeVoices);
			return *handle;
		}
		return {};
	}

	VoiceHandle AudioSubsystem::Play3D(std::string_view clipPath, const glm::vec3& position, const PlayParams& params)
	{
		const Expected<VoiceHandle> handle = PlayInternal(clipPath, params, true, position, false);
		if (handle.has_value())
		{
			AE_INFO(LogCategory::Audio, "Play3D {} at ({:.1f}, {:.1f}, {:.1f}) -> voice {} ({} active).", clipPath, position.x, position.y, position.z, handle->id, GetStats().activeVoices);
			return *handle;
		}
		return {};
	}

	bool AudioSubsystem::VoiceAlive(VoiceHandle handle) const
	{
		if (m_impl == nullptr)
		{
			return false;
		}
		const std::uint32_t slot = handle.id & 0xFFFFu;
		if (slot >= kMaxVoices)
		{
			return false;
		}
		const Impl::Voice& voice = m_impl->voices[slot];
		// Upper bit is the valid flag; the generation lives in bits 16..30 only.
		return voice.inUse && voice.generation == ((handle.id >> 16) & 0x7FFFu);
	}

	bool AudioSubsystem::IsVoiceAlive(VoiceHandle handle) const
	{
		return VoiceAlive(handle);
	}

	void AudioSubsystem::StopVoice(VoiceHandle handle)
	{
		if (!VoiceAlive(handle))
		{
			return;
		}
		Impl::Voice& voice = m_impl->voices[handle.id & 0xFFFFu];
		ma_sound_stop(&voice.sound);
		ma_sound_uninit(&voice.sound);
		voice.inUse = false;
	}

	void AudioSubsystem::SetVoicePosition(VoiceHandle handle, const glm::vec3& position)
	{
		if (!VoiceAlive(handle))
		{
			return;
		}
		Impl::Voice& voice = m_impl->voices[handle.id & 0xFFFFu];
		voice.position = position;
		if (voice.spatial)
		{
			ma_sound_set_position(&voice.sound, position.x, position.y, position.z);
		}
	}

	void AudioSubsystem::SetVoiceVelocity(VoiceHandle handle, const glm::vec3& velocity)
	{
		if (!VoiceAlive(handle))
		{
			return;
		}
		Impl::Voice& voice = m_impl->voices[handle.id & 0xFFFFu];
		voice.velocity = velocity;
		if (voice.spatial)
		{
			ma_sound_set_velocity(&voice.sound, velocity.x, velocity.y, velocity.z);
		}
	}

	void AudioSubsystem::PlayMusic(std::string_view clipPath, float fadeSeconds, bool loop, float volume)
	{
		if (m_impl == nullptr || m_impl->engine == nullptr)
		{
			return;
		}
		Impl& impl = *m_impl;
		ma_sound_group* group = impl.groups[static_cast<int>(Bus::Music)];
		if (group == nullptr)
		{
			return;
		}

		// First acquire the new track; only a successful load retires the old
		// one, so a typo'd path leaves the current music untouched.
		auto* sound = new ma_sound{};
		std::string path(clipPath);
		const ma_result result = ma_sound_init_from_file(impl.engine, path.c_str(), MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, group, nullptr, sound);
		if (result != MA_SUCCESS)
		{
			AE_WARN(LogCategory::Audio, "Failed to open music {}: {}", path, ma_result_description(result));
			delete sound;
			return;
		}

		ma_sound_set_looping(sound, loop ? MA_TRUE : MA_FALSE);
		ma_sound_set_volume(sound, 0.0f); // faded in by Update
		ma_sound_start(sound);

		// Whatever is currently fading out is finished off now; the current
		// track becomes the new outgoing one.
		if (impl.outgoing.sound != nullptr)
		{
			ma_sound_stop(impl.outgoing.sound);
			ma_sound_uninit(impl.outgoing.sound);
			delete impl.outgoing.sound;
		}
		impl.outgoing = impl.current;
		impl.outgoing.fadingOut = true;
		impl.outgoing.fadeElapsed = 0.0f;
		impl.outgoing.fadeSeconds = glm::max(fadeSeconds, 0.0f);

		impl.current = Impl::MusicTrack{.sound = sound, .fadingOut = false, .fadeElapsed = 0.0f, .fadeSeconds = glm::max(fadeSeconds, 0.0f), .volume = volume, .path = std::move(path)};
		AE_INFO(LogCategory::Audio, "PlayMusic {} (fade {:.2f}s).", impl.current.path, fadeSeconds);
	}

	void AudioSubsystem::StopMusic(float fadeSeconds)
	{
		if (m_impl == nullptr || m_impl->current.sound == nullptr)
		{
			return;
		}
		Impl& impl = *m_impl;
		if (fadeSeconds <= 0.0f)
		{
			ma_sound_stop(impl.current.sound);
			ma_sound_uninit(impl.current.sound);
			delete impl.current.sound;
			impl.current = {};
		}
		else
		{
			if (impl.outgoing.sound != nullptr)
			{
				ma_sound_stop(impl.outgoing.sound);
				ma_sound_uninit(impl.outgoing.sound);
				delete impl.outgoing.sound;
			}
			impl.outgoing = impl.current;
			impl.outgoing.fadingOut = true;
			impl.outgoing.fadeElapsed = 0.0f;
			impl.outgoing.fadeSeconds = fadeSeconds;
			impl.current = {};
		}
		AE_INFO(LogCategory::Audio, "StopMusic (fade {:.2f}s).", fadeSeconds);
	}

	void AudioSubsystem::SetBusVolume(Bus bus, float volume)
	{
		if (m_impl == nullptr)
		{
			return;
		}
		const auto index = std::clamp(static_cast<int>(bus), static_cast<int>(Bus::Music), static_cast<int>(Bus::Ambience));
		m_impl->busVolumes[index] = glm::clamp(volume, 0.0f, 1.0f);
		if (m_impl->groups[index] != nullptr)
		{
			ma_sound_group_set_volume(m_impl->groups[index], m_impl->busVolumes[index]);
		}
	}

	float AudioSubsystem::GetBusVolume(Bus bus) const
	{
		if (m_impl == nullptr)
		{
			return 0.0f;
		}
		return m_impl->busVolumes[std::clamp(static_cast<int>(bus), static_cast<int>(Bus::Music), static_cast<int>(Bus::Ambience))];
	}

	void AudioSubsystem::ApplySettings(const EngineSettings::Audio& settings)
	{
		if (m_impl == nullptr)
		{
			return;
		}
		m_impl->masterVolume = glm::clamp(settings.masterVolume, 0.0f, 1.0f);
		m_impl->muted = settings.muted;
		SetBusVolume(Bus::Music, settings.musicVolume);
		SetBusVolume(Bus::Sfx, settings.sfxVolume);
		SetBusVolume(Bus::Ambience, settings.ambienceVolume);
		// Master gain rides the endpoint so mute is instant for every voice.
		if (m_impl->engine != nullptr)
		{
			ma_engine_set_volume(m_impl->engine, EffectiveGain(1.0f, 1.0f, m_impl->masterVolume, m_impl->muted));
		}
	}

	void AudioSubsystem::SetListener(const ListenerPose& listener)
	{
		if (m_impl == nullptr || m_impl->engine == nullptr)
		{
			return;
		}
		m_impl->listener = listener;
		// Listener 0 is the single listener the engine mixes against.
		ma_engine_listener_set_position(m_impl->engine, 0, listener.position.x, listener.position.y, listener.position.z);
		ma_engine_listener_set_direction(m_impl->engine, 0, listener.forward.x, listener.forward.y, listener.forward.z);
		ma_engine_listener_set_world_up(m_impl->engine, 0, listener.up.x, listener.up.y, listener.up.z);
		ma_engine_listener_set_velocity(m_impl->engine, 0, listener.velocity.x, listener.velocity.y, listener.velocity.z);
	}

	void AudioSubsystem::PruneFinished()
	{
		Impl& impl = *m_impl;
		for (Impl::Voice& voice: impl.voices)
		{
			if (voice.inUse && !ma_sound_is_playing(&voice.sound) && ma_sound_at_end(&voice.sound))
			{
				ma_sound_uninit(&voice.sound);
				voice.inUse = false;
			}
		}
	}

	void AudioSubsystem::Update(float dt, const ListenerPose& listener)
	{
		if (m_impl == nullptr || m_impl->engine == nullptr)
		{
			return;
		}
		SetListener(listener);
		PruneFinished();
		Impl& impl = *m_impl;

		// NOTE: no per-voice mixing work here at all - miniaudio's spatializer
		// derives attenuation, panning, cone and doppler from the listener pose
		// (SetListener) and each sound's position/velocity (SetVoice*).

		// Music crossfades.
		for (Impl::MusicTrack* track: {&impl.current, &impl.outgoing})
		{
			if (track->sound == nullptr)
			{
				continue;
			}
			track->fadeElapsed += glm::max(dt, 0.0f);
			if (track->fadingOut)
			{
				const float gain = 1.0f - CrossfadeGain(track->fadeElapsed, track->fadeSeconds);
				ma_sound_set_volume(track->sound, gain * track->volume);
				if (track->fadeElapsed >= track->fadeSeconds)
				{
					ma_sound_stop(track->sound);
					ma_sound_uninit(track->sound);
					delete track->sound;
					*track = {};
				}
			}
			else
			{
				ma_sound_set_volume(track->sound, CrossfadeGain(track->fadeElapsed, track->fadeSeconds) * track->volume);
			}
		}
	}

	void AudioSubsystem::StopAllVoices()
	{
		if (m_impl == nullptr)
		{
			return;
		}
		for (Impl::Voice& voice: m_impl->voices)
		{
			if (voice.inUse)
			{
				ma_sound_stop(&voice.sound);
				ma_sound_uninit(&voice.sound);
				voice.inUse = false;
			}
		}
		// Level music is play state too: a snapshot restore must not leave it.
		if (m_impl->current.sound != nullptr)
		{
			ma_sound_stop(m_impl->current.sound);
			ma_sound_uninit(m_impl->current.sound);
			delete m_impl->current.sound;
			m_impl->current = {};
		}
		if (m_impl->outgoing.sound != nullptr)
		{
			ma_sound_stop(m_impl->outgoing.sound);
			ma_sound_uninit(m_impl->outgoing.sound);
			delete m_impl->outgoing.sound;
			m_impl->outgoing = {};
		}
	}

	void AudioSubsystem::SetSuspended(bool suspended)
	{
		if (m_impl == nullptr || m_impl->engine == nullptr || m_suspended == suspended)
		{
			return;
		}
		m_suspended = suspended;
		if (suspended)
		{
			ma_engine_stop(m_impl->engine);
		}
		else
		{
			ma_engine_start(m_impl->engine);
		}
	}

	Stats AudioSubsystem::GetStats() const
	{
		if (m_impl == nullptr)
		{
			return {};
		}
		Stats stats;
		stats.startedTotal = m_impl->startedTotal;
		stats.musicPlaying = m_impl->current.sound != nullptr;
		for (const Impl::Voice& voice: m_impl->voices)
		{
			stats.activeVoices += voice.inUse ? 1u : 0u;
		}
		return stats;
	}
} // namespace aether::audio
