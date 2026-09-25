#pragma once

#include <filesystem>
#include <span>
#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aether
{
	// Applied when MAILBOX is requested with no frame cap, which would otherwise run the loop
	// flat out. Deliberately above 60 so the extra headroom still buys latency, and low
	// enough that it is not a thermal event.
	// Default frame cap. MAILBOX discards whatever it renders past the display rate, so
	// shipping uncapped would burn a GPU to show the same 60 frames.
	inline constexpr float kDefaultTargetFps = 120.0f;

	struct EngineSettings
	{
		struct Window
		{
			int width = 2560;
			int height = 1440;
			// "windowed" | "borderless" | "fullscreen". Borderless is the one worth reaching
			// for: it is the only mode besides exclusive fullscreen that can win DWM
			// independent flip, and composition costs about a frame of latency. Unparsable
			// values fall back to windowed rather than guessing.
			std::string mode = "windowed";
		} window;

		struct Graphics
		{
			bool vsync = true;
			// How many frames the producer may run ahead of the screen. This is a LATENCY
			// control, not an allocation one: per-frame resources stay sized at
			// Swapchain::kMaxFramesInFlight, and this only throttles the game thread sooner.
			// Every frame of run-ahead is a vsync interval of input lag (~16.7 ms at 60 Hz),
			// so 3 costs ~50 ms; 2 trades a little CPU/GPU overlap for a frame of it, and 1
			// is lowest-latency but leaves the GPU idle while the CPU works. Clamped to
			// [1, Swapchain::kMaxFramesInFlight].
			int framesInFlight = 2;

			// Milliseconds before the predicted flip to latch input, matching Unreal's
			// rhi.SyncSlackMS (same default). Lower is more responsive and leaves less room
			// for a frame that runs long. The editor measured 0.92 ms of producer work and
			// 1.18 ms of GPU, so it has room to go well below the default.
			float syncSlackMs = 10.0f;
			// With vsync on, prefer MAILBOX over FIFO. Both are tear-free; FIFO makes each
			// present queue behind the last, while MAILBOX replaces the pending image, so a
			// frame reaches the screen without waiting its turn. Costs GPU work on frames
			// that get replaced, and does nothing when vsync is off (that is already
			// IMMEDIATE). Falls back to FIFO wherever the driver lacks MAILBOX.
			// On by default: MAILBOX still presents on the flip, so it does not tear, but a
			// finished frame replaces the pending one instead of queueing behind it. Measured
			// on the editor: present wait 16.6 ms -> 1.3 ms. The frames it can discard are
			// paid for by app.idleFps, which stops the loop running at all when untouched.
			bool lowLatencyPresent = true;
			// Idle out most of the display interval and latch input just before the flip that
			// will show the frame. Requires a MEASURED flip phase (VK_KHR_present_wait); where
			// that is unavailable this does nothing, because pacing against a guessed phase was
			// measured to be strictly worse than not pacing.
			// Off by default. Pacing deliberately locks the loop to the display cadence so it
			// can latch input late, which is the right trade for a display-locked game - it is
			// Unreal's r.GTSyncType 2, and Unreal does not use it for the editor either. With
			// MAILBOX the present no longer blocks, so the pacing buys latency back only to
			// spend it again on the slack.
			bool latencyPacing = false;

			// Render the SCENE at this fraction of the output and let the existing final
			// fullscreen pass upscale it; UI still draws at native resolution on top, so text
			// and sprites stay sharp. This is what makes borderless viable on a 4K panel,
			// where covering the output means 2.25x the pixels of a 1440p window.
			float renderScale = 1.0f;
			bool fxaa = true;
			// Colour grade. These defaults are the identity transform.
			float gradeContrast = 1.0f;
			float gradeSaturation = 1.0f;
			float gradeTemperature = 0.0f;
			float gradeTint = 0.0f;
			float vignetteIntensity = 0.0f;
			float vignetteRoundness = 1.0f;
			// Lateral chromatic aberration, in pixels of separation at the corner of frame.
			// Off by default: it is a look, and one nobody asked for is a regression.
			float chromaticAberration = 0.0f;
			// Contrast-adaptive sharpening, applied after FXAA. Off by default: FXAA is off
			// in some projects, and sharpening an already-sharp image only adds haloes.
			float sharpness = 0.0f;
			// Film grain. Off by default - it is a look, and one nobody asked for is a
			// regression.
			float filmGrain = 0.0f;
			// Camera motion blur: the shutter's open fraction of the frame interval, and the
			// ceiling on how far one frame may smear. Off by default - it is a look, and a
			// look nobody asked for is a regression.
			float motionBlur = 0.0f;
			float motionBlurMaxRadius = 64.0f;
			// Ambient occlusion. Radius is in world units - how far a surface looks for
			// something occluding it - so it wants to match the scale of the scene's
			// geometry rather than a fixed number of pixels.
			bool gtao = true;
			float gtaoRadius = 1.4f;
			float gtaoStrength = 1.35f;
			float specularFilter = 1.0f;
			float bloomStrength = 0.05f;
			float bloomRadius = 1.0f;
			bool autoExposure = true;
			float exposureKey = 0.18f;
			float exposureSpeed = 2.0f;
			float exposure = 1.0f;
			// Texture samples taken along the footprint when a surface is seen edge-on.
			// 1 disables it; the device ceiling is normally 16.
			int anisotropy = 16;
			// Constant added to every texture-sample LOD. The PS2 picked its mip from a fixed
			// per-draw LOD, not from screen-space derivatives, so low-resolution source art
			// (Twinsanity's 128px ground) reads sharper there than derivative LOD does; a
			// negative bias is the stand-in.
			float mipLodBias = 0.0f;
			// Short screen-space ray toward the sun, recovering the contact-scale occlusion
			// a shadow cascade texel is too coarse to hold.
			bool contactShadows = false;
			// Screen-space reflections.
			bool reflections = true;
			float reflectionMaxRoughness = 0.45f;
			float reflectionIntensity = 1.0f;
			// Display transform applied to the HDR image on the way to the screen. Stored
			// by NAME rather than by index: the operator list is a registry that grows, and
			// a project that picked one should not silently get a different look because
			// something was inserted above it. Matched case-insensitively against the names
			// in TonemapDefs.hpp; an unrecognised name falls back to the default rather than
			// leaving the screen black.
			// How the shadow cascades divide the view distance. 0 splits them evenly, 1
			// splits them logarithmically; the default sits between.
			//
			// This is a trade with no universally right answer, which is why it is a knob
			// rather than a constant. Measured as screen pixels covered by one shadow
			// texel - lower is crisper - at a 60 degree field of view:
			//
			//            1 m    10 m    60 m   200 m
			//   0.65    32.1     3.2     1.2     1.3
			//   0.90    10.0     1.0     4.5     1.3
			//
			// Raising it sharpens everything close to the camera and coarsens the middle
			// distance; past about 120 m the last cascade is unaffected either way. A game
			// whose subject is a few metres away wants it high, a vista wants it low.
			float shadowSplitLambda = 0.65f;
			// Marched, shadow-aware fog. Only ever active where a scene asked for it; this
			// is the machine-side off switch for when the march is too expensive.
			bool volumetrics = true;
			std::string tonemap = "ACES Filmic";
			bool asyncCompute = true;
			bool imguiViewports = true;
			float uiScale = 1.0f;
		} graphics;

		struct App
		{
			// Capped by default because MAILBOX is: without a cap the loop renders as fast as
			// it can and throws away most of it (a 2D game measured 3700 fps to put 60 on the
			// screen). 0 still means uncapped for anyone who explicitly asks for it.
			float targetFps = kDefaultTargetFps;

			// An editor nobody is touching has no reason to redraw at full rate. Unity's
			// Interaction Mode does the same thing: idle between frames, and stop idling the
			// moment the user interacts. 0 disables the throttle entirely.
			float idleFps = 10.0f;
			// How long after the last interaction to keep running at full rate. Covers the gap
			// between two keystrokes and animations that outlive the input that started them.
			float idleAfterSeconds = 0.75f;

			std::string startupScene;
			bool autoplay = false;
			// Editor autosave cadence, in seconds. Writes a recovery copy beside the project
			// (never over the scene itself) whenever there are unsaved edits. 0 disables it.
			float autosaveSeconds = 120.0f;
		} app;

		// The engine draws the mouse pointer itself when a project asks it to (see ui::CursorService),
		// which is how a game gets its own cursor without writing one. Off by default: tools and the
		// editor want the real OS pointer.
		struct Cursor
		{
			bool custom = false;
			std::string texture;       // VFS path to the pointer art; empty draws nothing
			float size = 32.0f;        // on-screen square size in pixels
			float hotspotX = 0.0f;     // 0..1 across the image: the pixel that sits on the mouse
			float hotspotY = 0.0f;
			bool pixelArt = true;      // nearest sampling, so small art scales up crisp
		} cursor;

		// Volume groups. All in 0..1 linear gain; a voice's final gain is
		// master * bus * voice volume (see audio::AudioMath::EffectiveGain).
		struct Audio
		{
			float masterVolume = 1.0f;
			float musicVolume = 1.0f;
			float sfxVolume = 1.0f;
			float ambienceVolume = 1.0f;
			bool muted = false;
		} audio;

		// NAT traversal servers, both plain IETF protocols (STUN: RFC 5389, TURN: RFC 5766 /
		// 8656) so any of these can point at self-hosted coturn, a friend's box, or a paid
		// provider - nothing here is tied to a vendor. See docs/multiplayer-relay.md.
		struct Network
		{
			// One-shot "what is my public address" lookup, no game traffic. A public default
			// is fine here - this is what NetTraversalSession used to hardcode at the call
			// site before it became a setting.
			std::string stunHost = "stun.l.google.com";
			int stunPort = 19302;

			// TURN relay: the fallback for a symmetric NAT, the one case a hole punch cannot
			// solve by construction. Unlike STUN's single lookup, every packet of the match
			// flows through this server, so - deliberately unlike stunHost above - there is
			// no default host, and allowRelay defaults off. Enabling a relay is a choice for
			// whoever configures the project or player to make, never one shipped for them.
			std::string turnHost;
			int turnPort = 3478;
			std::string turnUsername;
			std::string turnPassword;
			bool allowRelay = false;

			// Rendezvous server used to trade connect candidates with the other peer
			// when they are not on this network. Like turnHost there is no default,
			// because somebody has to run the box - but unlike a relay it costs almost
			// nothing to run and never sees game traffic: it forwards a few hundred
			// bytes of candidate addresses per join and forgets the room. Empty means
			// LAN play only, which needs no server at all. See tools/rendezvous/.
			std::string rendezvousHost;
			int rendezvousPort = 24701;
		} network;
	};

	[[nodiscard]] inline std::pair<std::string_view, std::string_view> SplitSettingKey(std::string_view key)
	{
		const auto dot = key.find('.');
		if (dot == std::string_view::npos)
		{
			return {std::string_view{}, key};
		}
		return {key.substr(0, dot), key.substr(dot + 1)};
	}

	// from this list, so they can never drift out of sync. Keep entries grouped by
	template<class S, class F>
	void ForEachSettingField(S& settings, F&& f)
	{
		f("window.width", settings.window.width);
		f("window.height", settings.window.height);
		f("window.mode", settings.window.mode);
		f("graphics.vsync", settings.graphics.vsync);
		f("graphics.framesInFlight", settings.graphics.framesInFlight);
		f("graphics.syncSlackMs", settings.graphics.syncSlackMs);
		f("graphics.lowLatencyPresent", settings.graphics.lowLatencyPresent);
		f("graphics.renderScale", settings.graphics.renderScale);
		f("graphics.latencyPacing", settings.graphics.latencyPacing);
		f("graphics.fxaa", settings.graphics.fxaa);
		f("graphics.gradeContrast", settings.graphics.gradeContrast);
		f("graphics.gradeSaturation", settings.graphics.gradeSaturation);
		f("graphics.gradeTemperature", settings.graphics.gradeTemperature);
		f("graphics.gradeTint", settings.graphics.gradeTint);
		f("graphics.vignetteIntensity", settings.graphics.vignetteIntensity);
		f("graphics.vignetteRoundness", settings.graphics.vignetteRoundness);
		f("graphics.chromaticAberration", settings.graphics.chromaticAberration);
		f("graphics.sharpness", settings.graphics.sharpness);
		f("graphics.filmGrain", settings.graphics.filmGrain);
		f("graphics.motionBlur", settings.graphics.motionBlur);
		f("graphics.motionBlurMaxRadius", settings.graphics.motionBlurMaxRadius);
		f("graphics.gtao", settings.graphics.gtao);
		f("graphics.gtaoRadius", settings.graphics.gtaoRadius);
		f("graphics.gtaoStrength", settings.graphics.gtaoStrength);
		f("graphics.specularFilter", settings.graphics.specularFilter);
		f("graphics.bloomStrength", settings.graphics.bloomStrength);
		f("graphics.bloomRadius", settings.graphics.bloomRadius);
		f("graphics.autoExposure", settings.graphics.autoExposure);
		f("graphics.exposureKey", settings.graphics.exposureKey);
		f("graphics.exposureSpeed", settings.graphics.exposureSpeed);
		f("graphics.exposure", settings.graphics.exposure);
		f("graphics.anisotropy", settings.graphics.anisotropy);
		f("graphics.mipLodBias", settings.graphics.mipLodBias);
		f("graphics.contactShadows", settings.graphics.contactShadows);
		f("graphics.reflections", settings.graphics.reflections);
		f("graphics.reflectionMaxRoughness", settings.graphics.reflectionMaxRoughness);
		f("graphics.reflectionIntensity", settings.graphics.reflectionIntensity);
		f("graphics.shadowSplitLambda", settings.graphics.shadowSplitLambda);
		f("graphics.volumetrics", settings.graphics.volumetrics);
		f("graphics.tonemap", settings.graphics.tonemap);
		f("graphics.asyncCompute", settings.graphics.asyncCompute);
		f("graphics.imguiViewports", settings.graphics.imguiViewports);
		f("graphics.uiScale", settings.graphics.uiScale);
		f("app.targetFps", settings.app.targetFps);
		f("app.idleFps", settings.app.idleFps);
		f("app.idleAfterSeconds", settings.app.idleAfterSeconds);
		f("app.startupScene", settings.app.startupScene);
		f("app.autoplay", settings.app.autoplay);
		f("app.autosaveSeconds", settings.app.autosaveSeconds);
		f("cursor.custom", settings.cursor.custom);
		f("cursor.texture", settings.cursor.texture);
		f("cursor.size", settings.cursor.size);
		f("cursor.hotspotX", settings.cursor.hotspotX);
		f("cursor.hotspotY", settings.cursor.hotspotY);
		f("cursor.pixelArt", settings.cursor.pixelArt);
		f("network.stunHost", settings.network.stunHost);
		f("network.stunPort", settings.network.stunPort);
		f("network.turnHost", settings.network.turnHost);
		f("network.turnPort", settings.network.turnPort);
		f("network.turnUsername", settings.network.turnUsername);
		f("network.turnPassword", settings.network.turnPassword);
		f("network.allowRelay", settings.network.allowRelay);
		f("network.rendezvousHost", settings.network.rendezvousHost);
		f("network.rendezvousPort", settings.network.rendezvousPort);
		f("audio.masterVolume", settings.audio.masterVolume);
		f("audio.musicVolume", settings.audio.musicVolume);
		f("audio.sfxVolume", settings.audio.sfxVolume);
		f("audio.ambienceVolume", settings.audio.ambienceVolume);
		f("audio.muted", settings.audio.muted);
	}

	// What a settings key means, what it will accept, and whether it needs a restart -
	// the part the reflection above cannot carry, because f() only sees a name and a
	// reference. Kept immediately beside ForEachSettingField so a new setting's help text
	// is added where its key is; a key with no entry still renders, just without help.
	//
	// The field comments on EngineSettings are the long form of these; this is the line
	// that fits in a tooltip.
	struct SettingInfo
	{
		std::string_view description;
		// Inclusive bounds for a numeric setting. Equal values mean unbounded.
		double minValue = 0.0;
		double maxValue = 0.0;
		// Closed value set for a string setting; empty means free text.
		std::span<const std::string_view> choices;
		// Read once at startup, so editing it does nothing until the next launch.
		bool restartRequired = false;
	};

	[[nodiscard]] const SettingInfo& SettingMetadata(std::string_view key);

	// Value a setting has in a default-constructed EngineSettings, for "reset this field".
	// Returns false when the key does not exist or does not hold a T.
	template<class T>
	bool DefaultSettingValue(std::string_view key, T& out)
	{
		static const EngineSettings kDefaults{};
		bool found = false;
		ForEachSettingField(kDefaults,
		        [&](std::string_view candidate, const auto& field)
		        {
			        if (candidate != key || found)
			        {
				        return;
			        }
			        if constexpr (std::is_same_v<std::decay_t<decltype(field)>, T>)
			        {
				        out = field;
				        found = true;
			        }
		        });
		return found;
	}

	// Write a setting by key, from text.
	//
	// The counterpart to DefaultSettingValue, and the reason it takes a STRING rather than a
	// typed value: the callers that need this - a control endpoint, a config importer - have
	// text and no idea which of bool/int/float/string the key holds. Reflection knows, so
	// the parse belongs here rather than duplicated at every call site.
	//
	// Returns false when the key does not exist or the text does not parse as its type. It
	// does NOT apply the change; call SettingsService::ApplyField afterwards, which is the
	// same two-step the settings UI follows.
	[[nodiscard]] inline bool SetSettingValueFromString(EngineSettings& settings, std::string_view key, std::string_view text)
	{
		bool assigned = false;
		ForEachSettingField(settings,
		        [&](std::string_view candidate, auto& field)
		        {
			        if (candidate != key || assigned)
			        {
				        return;
			        }
			        using Field = std::decay_t<decltype(field)>;
			        if constexpr (std::is_same_v<Field, bool>)
			        {
				        if (text == "true" || text == "1") { field = true; assigned = true; }
				        else if (text == "false" || text == "0") { field = false; assigned = true; }
			        }
			        else if constexpr (std::is_same_v<Field, std::string>)
			        {
				        // A key with a closed value set gets checked against it. The metadata
				        // exists so a value the loader would silently reject cannot be entered;
				        // accepting one here just moves the rejection to the next launch, where
				        // it reads as the setting having been forgotten.
				        const std::span<const std::string_view> choices = SettingMetadata(key).choices;
				        if (choices.empty() || std::find(choices.begin(), choices.end(), text) != choices.end())
				        {
					        field = std::string(text);
					        assigned = true;
				        }
			        }
			        else if constexpr (std::is_integral_v<Field> || std::is_floating_point_v<Field>)
			        {
				        Field parsed{};
				        if (std::from_chars(text.data(), text.data() + text.size(), parsed).ec == std::errc{})
				        {
					        // Clamped rather than rejected, to match what the loader does with
					        // the same value out of a settings file.
					        const SettingInfo& info = SettingMetadata(key);
					        if (info.minValue < info.maxValue)
					        {
						        const double clamped = std::min(std::max(static_cast<double>(parsed), info.minValue), info.maxValue);
						        parsed = static_cast<Field>(clamped);
					        }
					        field = parsed;
					        assigned = true;
				        }
			        }
		        });
		return assigned;
	}

	// Read a setting by key as text, so a caller can show or round-trip a value without
	// knowing its type.
	[[nodiscard]] inline bool GetSettingValueAsString(const EngineSettings& settings, std::string_view key, std::string& out)
	{
		bool found = false;
		ForEachSettingField(settings,
		        [&](std::string_view candidate, const auto& field)
		        {
			        if (candidate != key || found)
			        {
				        return;
			        }
			        using Field = std::decay_t<decltype(field)>;
			        if constexpr (std::is_same_v<Field, bool>)
			        {
				        out = field ? "true" : "false";
			        }
			        else if constexpr (std::is_same_v<Field, std::string>)
			        {
				        out = field;
			        }
			        else
			        {
				        out = std::to_string(field);
			        }
			        found = true;
		        });
		return found;
	}

	// Keys that belong to the project, never to the machine. The startup scene is the one
	// that matters: it decides what a published game boots, and the bake reads the project
	// layer only. Letting it sit in the per-user file gives an editor that boots the right
	// scene on the machine that set it and an empty world everywhere else - including in
	// every published build. So the user layer neither writes these nor reads them back.
	[[nodiscard]] inline bool IsProjectOnlySettingKey(std::string_view key) noexcept
	{
		return key == "app.startupScene";
	}

	// Where the EDITOR saves a setting the user changed.
	//
	// Deliberately a separate question from IsProjectOnlySettingKey above, which asks whether
	// a per-user file is ALLOWED to override a key. A player tuning quality on their own
	// machine is legitimate, so most of these stay user-overridable; what changes here is only
	// which file the editor writes them to.
	//
	// Project keys describe the GAME - how it looks and how it boots - so they belong beside
	// the scenes in source control and must travel with a published build. User keys describe
	// one machine: its window, how it presents frames, how the editor's own chrome is scaled.
	// Before this split every edit went to the per-user file, which publishing deliberately
	// ignores, so an authored look silently failed to ship.
	enum class SettingsHome
	{
		User,
		Project,
	};

	[[nodiscard]] inline SettingsHome SettingsHomeFor(std::string_view key) noexcept
	{
		// Window geometry, presentation and pacing, the editor's own UI, and the NAT
		// traversal servers all describe the machine sitting in front of the project, not
		// the project - a relay is whoever is running the engine's own choice to make (see
		// docs/multiplayer-relay.md), never something a shipped project decides for them.
		if (key == "window.width" || key == "window.height" || key == "window.mode"
		        || key == "graphics.vsync" || key == "graphics.framesInFlight" || key == "graphics.lowLatencyPresent"
		        || key == "graphics.renderScale" || key == "graphics.latencyPacing" || key == "graphics.syncSlackMs" || key == "graphics.asyncCompute"
		        || key == "graphics.anisotropy" || key == "graphics.imguiViewports" || key == "graphics.uiScale"
		        || key == "app.targetFps" || key == "app.idleFps" || key == "app.idleAfterSeconds" || key == "app.autosaveSeconds"
		        || key == "network.stunHost" || key == "network.stunPort" || key == "network.turnHost" || key == "network.turnPort"
		        || key == "network.turnUsername" || key == "network.turnPassword" || key == "network.allowRelay"
		        || key == "network.rendezvousHost" || key == "network.rendezvousPort")
		{
			return SettingsHome::User;
		}
		// Everything else - the tonemap and grade, the vignette, ambient occlusion,
		// reflections, shadows, the startup scene, the cursor - is authored, and ships.
		return SettingsHome::Project;
	}

	// Which keys a document is allowed to contribute. UserOverridable drops the
	// project-only keys above, so a stale machine-local file can never mask the project.
	enum class SettingsScope
	{
		All,
		UserOverridable,
	};

	struct LoadedEngineSettings
	{
		EngineSettings values;
		// Everything below the per-user file: defaults, then shipped, then project. What a
		// user override is measured against.
		EngineSettings base;
		// Defaults plus the shipped file only. What a PROJECT override is measured against,
		// so a project file records what it changed about the engine rather than restating
		// every default and pinning it against future engine changes.
		EngineSettings shipped;
	};

	// the base (1+2+3), so keys the user never touched keep tracking shipped/project
	class EngineSettingsIO
	{
	public:
		[[nodiscard]] static LoadedEngineSettings LoadLayered(std::string_view shippedFile = "EngineSettings.toml", const std::filesystem::path& projectFile = {}, std::string_view userFile = "UserSettings.toml");

		[[nodiscard]] static EngineSettings LoadOrCreate(std::string_view shippedFile = "EngineSettings.toml", const std::filesystem::path& projectFile = {}, std::string_view userFile = "UserSettings.toml");

		// settings file (io::PlatformPaths::GetUserConfigDir()/userFile). Never
		static void SaveUserOverrides(const EngineSettings& settings, const EngineSettings& base, std::string_view userFile = "UserSettings.toml");

		// Merges the project-homed settings that differ from the shipped baseline into an
		// existing ProjectSettings.toml, leaving its other sections untouched. Refuses to
		// write a file it could not read or parse rather than replacing a project's paths and
		// publish config with a handful of graphics keys.
		static bool SaveProjectOverrides(const EngineSettings& settings, const EngineSettings& shippedBase, const std::filesystem::path& projectFile, std::string& error);

		static void Apply(std::string_view tomlText, EngineSettings& settings, SettingsScope scope = SettingsScope::All);

		// Clamps fields to valid ranges after a merge (e.g. window dimensions must
		static void Sanitize(EngineSettings& settings);

		[[nodiscard]] static std::string Serialize(const EngineSettings& settings);

		[[nodiscard]] static std::string SerializeOverrides(const EngineSettings& settings, const EngineSettings& base);

		// (release layout) over working-directory-relative ones (dev layout).
		[[nodiscard]] static std::filesystem::path ResolvePath(std::string_view fileName = "EngineSettings.toml");
	};
} // namespace aether
