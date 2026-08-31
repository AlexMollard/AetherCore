#include "utils/EngineSettings.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/TomlConfig.hpp"

#include <sstream>
#include "utils/Profiler.hpp"
#include "utils/TextIni.hpp"
#include "passes/TonemapDefs.hpp"

namespace aether
{
	namespace
	{
		constexpr std::array<std::string_view, 3> kWindowModes{"windowed", "borderless", "fullscreen"};

		// One line per key. Ranges are what the engine will actually honour, so the UI can
		// stop a value being typed that the loader would silently clamp or reject.
		const std::array kSettingInfo = std::to_array<std::pair<std::string_view, SettingInfo>>({
		        {"window.width", {.description = "Window width in pixels. Ignored in borderless and fullscreen, which match the display.", .minValue = 320.0, .maxValue = 16384.0}},
		        {"window.height", {.description = "Window height in pixels. Ignored in borderless and fullscreen, which match the display.", .minValue = 240.0, .maxValue = 16384.0}},
		        {"window.mode", {.description = "Borderless is the only mode besides fullscreen that can win DWM independent flip; composition costs about a frame of latency.", .choices = kWindowModes}},
		        {"graphics.vsync", {.description = "Wait for the display to refresh. Turning it off tears, but removes a frame of latency."}},
		        {"graphics.framesInFlight", {.description = "How far the game thread may run ahead of the screen. Every frame of run-ahead is one display interval of input lag (~17 ms at 60 Hz).", .minValue = 1.0, .maxValue = 3.0}},
		        {"graphics.lowLatencyPresent", {.description = "Prefer MAILBOX over FIFO while vsync is on: a finished frame replaces the pending one instead of queueing behind it."}},
		        {"graphics.latencyPacing", {.description = "Idle out most of the display interval and latch input just before the flip. Needs a measured flip phase; does nothing without one."}},
		        {"graphics.renderScale", {.description = "Scene resolution as a multiple of the output. Below 1 renders small and upscales, for performance. Above 1 renders large and downsamples - supersampling, the bluntest and best antialiasing, at 4x the pixels for 2x. UI always draws at native resolution.", .minValue = 0.25, .maxValue = 2.0}},
		        {"graphics.fxaa", {.description = "Cheap post-process antialiasing."}},
		        {"graphics.gradeContrast", {.description = "Contrast about middle grey, applied in linear light before tonemapping. 1 leaves the image untouched.", .minValue = 0.25, .maxValue = 2.5}},
		        {"graphics.gradeSaturation", {.description = "Colour saturation. 1 leaves the image untouched, 0 is greyscale, above 1 pushes further from grey.", .minValue = 0.0, .maxValue = 2.5}},
		        {"graphics.gradeTemperature", {.description = "White balance along the warm/cool axis. Negative is cooler (bluer), positive warmer (redder); 0 is neutral. Luminance is preserved, so this does not double as an exposure control.", .minValue = -1.0, .maxValue = 1.0}},
		        {"graphics.vignetteIntensity", {.description = "Lens falloff toward the edge of frame, applied in linear light so corner highlights roll off rather than just dimming. 0 is off.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.motionBlur", {.description = "Camera motion blur: how much of the frame the shutter stays open for. 0.5 is the film convention (a 180-degree shutter); 0 is off. Blurs camera movement only, not moving objects.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.motionBlurMaxRadius", {.description = "Ceiling on how far one frame may smear, in pixels. Bounds both the cost and the streak a camera cut would otherwise leave.", .minValue = 8.0, .maxValue = 256.0}},
		        {"graphics.vignetteRoundness", {.description = "1 makes the falloff circular on screen; 0 follows the aspect ratio, darkening the sides of a wide frame rather than only the corners.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.chromaticAberration", {.description = "Lens colour fringing, in pixels of red/blue separation at the corner of frame. Zero at the centre and strongest at the edges, as on a real lens. 0 is off.", .minValue = 0.0, .maxValue = 16.0}},
		        {"graphics.sharpness", {.description = "Contrast-adaptive sharpening applied after antialiasing, to recover the detail FXAA softens. Scaled by local contrast, so flat areas stay clean and edges near black or white do not halo. 0 is off.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.filmGrain", {.description = "Film grain, animated per frame. Strongest in the midtones and fading out of the blacks and the highlights, the way film emulsion actually grains. 0 is off.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.gradeTint", {.description = "White balance along the green/magenta axis. Negative is magenta, positive green; 0 is neutral.", .minValue = -1.0, .maxValue = 1.0}},
		        {"graphics.gtao", {.description = "Ground-truth ambient occlusion: darkens creases and contact points that the light probes cannot see into."}},
		        {"graphics.gtaoRadius", {.description = "How far, in world units, a surface looks for geometry occluding it. Too small and only tight creases darken; too large and the whole scene greys.", .minValue = 0.1, .maxValue = 5.0}},
		        {"graphics.gtaoStrength", {.description = "How hard the occlusion is applied. Affects ambient light only, never direct light.", .minValue = 0.0, .maxValue = 3.0}},
		        {"graphics.autoExposure",
		                {.description = "Adapt exposure to the scene's own brightness, the way an eye does. Off holds the manual exposure instead - which is also what makes two captures of the same scene comparable."}},
		        {"graphics.exposureKey",
		                {.description = "The average brightness auto-exposure drives the image toward. Higher is a brighter picture.", .minValue = 0.01, .maxValue = 1.0}},
		        {"graphics.exposureSpeed",
		                {.description = "How fast auto-exposure adapts, in e-folds per second. Low is a slow, cinematic settle; high snaps.", .minValue = 0.05, .maxValue = 8.0}},
		        {"graphics.exposure",
		                {.description = "Manual exposure multiplier, applied on top of auto-exposure and used alone when it is off.", .minValue = 0.01, .maxValue = 16.0}},
		        {"graphics.bloomStrength",
		                {.description = "How much of the bright pass is added back over the image. 0 disables bloom.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.bloomRadius",
		                {.description = "How wide each bloom mip is sampled when the chain is walked back up. Larger is a softer, further-reaching glow.", .minValue = 0.5, .maxValue = 4.0}},
		        {"graphics.cloudCoverage",
		                {.description = "How much of the visible sky is cloud. 0 is a clear sky and costs nothing. Affects only what the camera sees - the ambient light every surface samples stays the authored sky.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.cloudSpeed",
		                {.description = "How fast the cloud layer drifts.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.specularFilter",
		                {.description = "How strongly roughness is widened to hide specular aliasing - the sparkle on normal-mapped and curved surfaces at distance. 1 is the full filter; 0 leaves the highlight unfiltered and sparkling.",
		                        .minValue = 0.0,
		                        .maxValue = 1.0}},
		        {"graphics.reflections", {.description = "Screen-space reflections: marches the depth buffer so surfaces reflect what is actually in front of them instead of only the sky."}},
		        {"graphics.reflectionMaxRoughness", {.description = "Roughness above which reflections fall back to the sky probe. A wide lobe needs many rays to look like anything but noise.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.reflectionIntensity", {.description = "Strength of the screen-space reflection contribution.", .minValue = 0.0, .maxValue = 2.0}},
		        {"graphics.contactShadows", {.description = "Recovers the small shadow at the base of an object that a shadow cascade texel is too coarse to resolve, by marching a short ray through the depth buffer."}},
		        {"graphics.anisotropy", {.description = "Texture samples taken along the footprint when a surface is seen edge-on. 1 disables it, which visibly blurs ground at a grazing angle.", .minValue = 1.0, .maxValue = 16.0}},
		        {"graphics.shadowSplitLambda", {.description = "How the shadow cascades divide the view distance. 0 is even, 1 is logarithmic. Higher sharpens shadows near the camera and coarsens the middle distance; the far cascade is unaffected.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.volumetrics", {.description = "March the sun's shadow cascades through the scene's fog so shadows carve beams out of it. Costs nothing in a scene with no fog authored."}},
		        {"graphics.tonemap", {.description = "Display transform applied to the HDR image. ACES rolls highlights toward white in AP1; AgX mixes the channels before compressing them, so bright saturated colour stops rotating hue on its way to white.", .choices = kTonemapNames}},
		        {"graphics.asyncCompute", {.description = "Overlap compute work with graphics on a separate queue. Does nothing on a device without a dedicated compute queue."}},
		        {"graphics.imguiViewports", {.description = "Let editor panels become separate OS windows when dragged out of the main window."}},
		        {"graphics.uiScale", {.description = "Extra multiplier on editor UI size, on top of the display's own DPI scale.", .minValue = 0.5, .maxValue = 3.0}},
		        {"app.targetFps", {.description = "Frame cap. 0 leaves it uncapped.", .minValue = 0.0, .maxValue = 1000.0}},
		        {"app.startupScene", {.description = "Scene a published build boots into.", .restartRequired = true}},
		        {"app.autoplay", {.description = "Start the game straight away instead of opening the editor.", .restartRequired = true}},
		        {"app.autosaveSeconds", {.description = "How often the editor writes a recovery copy of unsaved scene edits, beside the project and never over the scene itself. 0 disables it.", .minValue = 0.0, .maxValue = 3600.0}},
		        {"cursor.custom", {.description = "Draw the mouse pointer from a texture instead of using the OS pointer."}},
		        {"cursor.texture", {.description = "VFS path to the pointer art. Empty draws nothing."}},
		        {"cursor.size", {.description = "On-screen pointer size, in pixels.", .minValue = 4.0, .maxValue = 256.0}},
		        {"cursor.hotspotX", {.description = "Which point of the image sits under the mouse, 0..1 across the width.", .minValue = 0.0, .maxValue = 1.0}},
		        {"cursor.hotspotY", {.description = "Which point of the image sits under the mouse, 0..1 down the height.", .minValue = 0.0, .maxValue = 1.0}},
		        {"cursor.pixelArt", {.description = "Nearest-neighbour sampling, so small pointer art scales up crisp."}},
		});

		template<class T>
		void AssignField(T& field, std::string_view raw)
		{
			if constexpr (std::is_same_v<T, bool>)
			{
				if (const auto parsed = text::ParseBool(raw))
				{
					field = *parsed;
				}
			}
			else if constexpr (std::is_same_v<T, int>)
			{
				if (const auto parsed = text::ParseInt(raw))
				{
					field = *parsed;
				}
			}
			else if constexpr (std::is_same_v<T, float>)
			{
				if (const auto parsed = text::ParseFloat(raw))
				{
					field = *parsed;
				}
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
				field = text::StripQuotes(std::string(raw));
			}
			else
			{
				static_assert(sizeof(T) == 0, "ForEachSettingField uses a field type with no AssignField branch");
			}
		}

		template<class T>
		std::string FormatField(const T& field)
		{
			if constexpr (std::is_same_v<T, bool>)
			{
				return field ? "true" : "false";
			}
			else if constexpr (std::is_same_v<T, std::string>)
			{
				return "\"" + field + "\"";
			}
			else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, float>)
			{
				std::ostringstream value;
				value << field;
				return value.str();
			}
			else
			{
				static_assert(sizeof(T) == 0, "ForEachSettingField uses a field type with no FormatField branch");
			}
		}
	} // namespace

	void EngineSettingsIO::Apply(std::string_view tomlText, EngineSettings& settings, const SettingsScope scope)
	{
		text::ParseToml(tomlText,
		        [&settings, scope](const text::IniEntry& entry)
		        {
			        ForEachSettingField(settings,
			                [&entry, scope](std::string_view key, auto& field)
			                {
				                if (scope == SettingsScope::UserOverridable && IsProjectOnlySettingKey(key))
				                {
					                return;
				                }
				                if (text::ToLowerAscii(std::string(key)) == entry.fullKey)
				                {
					                AssignField(field, entry.value);
				                }
			                });
		        });
	}

	void EngineSettingsIO::Sanitize(EngineSettings& settings)
	{
		const EngineSettings defaults{};
		if (settings.window.width < 1)
		{
			settings.window.width = defaults.window.width;
		}
		if (settings.window.height < 1)
		{
			settings.window.height = defaults.window.height;
		}
		settings.app.targetFps = std::max(0.0f, settings.app.targetFps);
		settings.graphics.framesInFlight = std::clamp(settings.graphics.framesInFlight, 1, 3);
		settings.graphics.uiScale = std::clamp(settings.graphics.uiScale, 0.5f, 3.0f);
		// Below a quarter the scene is unrecognisable, and above 1 it would be supersampling
		// rather than the cost saving this exists for.
		settings.graphics.renderScale = std::clamp(settings.graphics.renderScale, 0.25f, 2.0f);

		// MAILBOX never blocks the producer, so with no frame cap the loop runs as fast as it
		// possibly can - a 2D game measured 3700 fps to put 60 on the screen, discarding 98%
		// of them. That is a melted GPU for no visible benefit, and it is the state a user
		// lands in by flipping one setting, so it cannot be left to documentation.
		if (settings.graphics.vsync && settings.graphics.lowLatencyPresent && settings.app.targetFps <= 0.0f)
		{
			settings.app.targetFps = kUncappedMailboxFallbackFps;
		}
	}

	std::string EngineSettingsIO::Serialize(const EngineSettings& settings)
	{
		std::ostringstream out;
		out << "# AetherCore settings (TOML)\n";

		std::string currentSection;
		ForEachSettingField(settings,
		        [&](std::string_view key, const auto& field)
		        {
			        const auto [section, name] = SplitSettingKey(key);
			        if (section != currentSection)
			        {
				        currentSection = std::string(section);
				        out << "\n[" << currentSection << "]\n";
			        }
			        out << name << " = " << FormatField(field) << "\n";
		        });
		return out.str();
	}

	std::string EngineSettingsIO::SerializeOverrides(const EngineSettings& settings, const EngineSettings& base)
	{
		struct FieldLine
		{
			std::string_view section;
			std::string_view name;
			std::string value;
			bool projectOnly;
		};

		std::vector<FieldLine> current;
		std::vector<std::string> baseline;
		ForEachSettingField(settings,
		        [&](std::string_view key, const auto& field)
		        {
			        const auto [section, name] = SplitSettingKey(key);
			        const bool belongsToProject = IsProjectOnlySettingKey(key) || SettingsHomeFor(key) == SettingsHome::Project;
			        current.push_back({section, name, FormatField(field), belongsToProject});
		        });
		ForEachSettingField(base, [&](std::string_view, const auto& field) { baseline.push_back(FormatField(field)); });

		std::ostringstream out;
		out << "# AetherCore user settings (TOML)\n";
		out << "# Overrides layered on top of the shipped EngineSettings.toml; only changed keys are stored.\n";

		std::string currentSection;
		for (std::size_t i = 0; i < current.size(); ++i)
		{
			// Keys that live in the project never enter the per-user file, even when they
			// differ from the base. Writing them here is what made an authored look fail to
			// ship: publishing reads shipped+project and ignores this file entirely. Any
			// such key left over from an older build is dropped the first time this runs.
			if (current[i].projectOnly || current[i].value == baseline[i])
			{
				continue;
			}
			if (current[i].section != currentSection)
			{
				currentSection = std::string(current[i].section);
				out << "\n[" << currentSection << "]\n";
			}
			out << current[i].name << " = " << current[i].value << "\n";
		}
		return out.str();
	}

	bool EngineSettingsIO::SaveProjectOverrides(const EngineSettings& settings, const EngineSettings& shippedBase, const std::filesystem::path& projectFile, std::string& error)
	{
		if (projectFile.empty())
		{
			error = "No project is open.";
			return false;
		}

		// Read and parse first, and refuse on either failure. A project file holds paths, the
		// project name and the startup scene; replacing all of that with a handful of
		// graphics keys because it happened to be locked or malformed would be far worse than
		// declining to save. Same guard as WriteProjectStartupScene, for the same reason.
		TomlConfig config;
		{
			auto text = io::file_util::ReadText(projectFile);
			std::error_code ec;
			if (!text && std::filesystem::exists(projectFile, ec))
			{
				error = "Could not read project settings; refusing to overwrite " + projectFile.generic_string();
				return false;
			}
			if (text && !config.Load(*text))
			{
				error = "Could not parse project settings; refusing to overwrite " + projectFile.generic_string();
				return false;
			}
		}

		// Walk both structs in lockstep. ForEachSettingField visits in a fixed order, so an
		// index is enough to pair a value with its shipped counterpart.
		std::vector<std::string> shippedValues;
		ForEachSettingField(shippedBase, [&](std::string_view, const auto& field) { shippedValues.push_back(FormatField(field)); });

		std::size_t index = 0;
		ForEachSettingField(settings,
		        [&](std::string_view key, const auto& field)
		        {
			        const std::size_t i = index++;
			        if (SettingsHomeFor(key) != SettingsHome::Project)
			        {
				        return;
			        }

			        // Back at the engine's own value: drop the key rather than restating it.
			        // A project that pins every default would stop tracking engine changes,
			        // and the file would say nothing about what the project actually chose.
			        if (i < shippedValues.size() && FormatField(field) == shippedValues[i])
			        {
				        config.Erase(key);
				        return;
			        }

			        using FieldType = std::decay_t<decltype(field)>;
			        if constexpr (std::is_same_v<FieldType, bool>)
			        {
				        config.Set(key, field);
			        }
			        else if constexpr (std::is_same_v<FieldType, float>)
			        {
				        config.Set(key, field);
			        }
			        else if constexpr (std::is_same_v<FieldType, int>)
			        {
				        config.Set(key, field);
			        }
			        else if constexpr (std::is_same_v<FieldType, std::string>)
			        {
				        config.Set(key, std::string_view{field});
			        }
			        else
			        {
				        // ForEachSettingField instantiates this for EVERY field type, not just
				        // the project-homed ones, so a type with no writer here is a build
				        // error rather than a key silently missing from the file that ships.
				        static_assert(std::is_same_v<FieldType, bool> || std::is_same_v<FieldType, float>
				                        || std::is_same_v<FieldType, int> || std::is_same_v<FieldType, std::string>,
				                "SaveProjectOverrides has no writer for this setting field type");
			        }
		        });

		std::ostringstream buffer;
		config.Save(buffer, "AetherCore project file.");
		if (auto result = io::file_util::WriteText(projectFile, buffer.str()); !result)
		{
			error = "Could not write project settings: " + result.error().message;
			return false;
		}
		return true;
	}

	std::filesystem::path EngineSettingsIO::ResolvePath(std::string_view fileName)
	{
		std::filesystem::path requested(fileName);
		if (requested.is_absolute())
		{
			return requested;
		}

		const auto exeDir = io::PlatformPaths::GetExecutableDir();
		std::error_code ec;
		const auto cwd = std::filesystem::current_path(ec);

		std::vector<std::filesystem::path> candidates;
		if (!exeDir.empty())
		{
			candidates.push_back(exeDir / "data" / "config" / requested);
			candidates.push_back(exeDir / "config" / requested);
			candidates.push_back(exeDir / requested);
		}
		if (!ec)
		{
			candidates.push_back(cwd / "data" / "config" / requested);
			candidates.push_back(cwd / ".." / "data" / "config" / requested);
			candidates.push_back(cwd / ".." / ".." / "data" / "config" / requested);
			candidates.push_back(cwd / "config" / requested);
			candidates.push_back(cwd / ".." / "config" / requested);
			candidates.push_back(cwd / ".." / ".." / "config" / requested);
			candidates.push_back(cwd / requested);
		}

		for (const auto& candidate: candidates)
		{
			if (io::file_util::Exists(candidate))
			{
				return candidate;
			}
		}
		return candidates.empty() ? requested : candidates.front();
	}

	LoadedEngineSettings EngineSettingsIO::LoadLayered(std::string_view shippedFile, const std::filesystem::path& projectFile, std::string_view userFile)
	{
		AE_PROFILE_ZONE();
		LoadedEngineSettings result;

		const auto shippedPath = ResolvePath(shippedFile);
		if (auto text = io::file_util::ReadText(shippedPath))
		{
			Apply(*text, result.values);
			AE_INFO(LogCategory::Engine, "Shipped settings loaded from {}", shippedPath.string());
		}
		else
		{
			AE_INFO(LogCategory::Engine, "No shipped settings file at {}; using compiled-in defaults.", shippedPath.string());
		}

		// Snapshot before the project layer: a project override is measured against the
		// engine's own shipped values.
		Sanitize(result.values);
		result.shipped = result.values;

		if (!projectFile.empty())
		{
			if (auto text = io::file_util::ReadText(projectFile))
			{
				Apply(*text, result.values);
				AE_INFO(LogCategory::Engine, "Project settings loaded from {}", projectFile.string());
			}
		}

		Sanitize(result.values);
		result.base = result.values;

		if (const auto userDir = io::PlatformPaths::GetUserConfigDir(); !userDir.empty())
		{
			const auto userPath = userDir / userFile;
			if (auto text = io::file_util::ReadText(userPath))
			{
				Apply(*text, result.values, SettingsScope::UserOverridable);
				AE_INFO(LogCategory::Engine, "User settings overrides loaded from {}", userPath.string());
			}
		}
		Sanitize(result.values);

		const auto& v = result.values;
		AE_INFO(LogCategory::Engine,
		        "Settings resolved ({}x{}, VSync={}, FXAA={}, AsyncCompute={}, TargetFPS={})",
		        v.window.width,
		        v.window.height,
		        v.graphics.vsync ? "on" : "off",
		        v.graphics.fxaa ? "on" : "off",
		        v.graphics.asyncCompute ? "on" : "off",
		        v.app.targetFps);
		return result;
	}

	EngineSettings EngineSettingsIO::LoadOrCreate(std::string_view shippedFile, const std::filesystem::path& projectFile, std::string_view userFile)
	{
		return LoadLayered(shippedFile, projectFile, userFile).values;
	}

	void EngineSettingsIO::SaveUserOverrides(const EngineSettings& settings, const EngineSettings& base, std::string_view userFile)
	{
		AE_PROFILE_ZONE();
		const auto userDir = io::PlatformPaths::GetUserConfigDir();
		if (userDir.empty())
		{
			AE_WARN(LogCategory::Engine, "No user config directory available; settings not saved.");
			return;
		}

		const auto path = userDir / userFile;
		const std::string text = SerializeOverrides(settings, base);

		if (auto result = io::file_util::WriteText(path, text); !result)
		{
			AE_WARN(LogCategory::Engine, "Failed to write user settings file: {} - {}", path.string(), result.error().message);
			return;
		}
		AE_INFO(LogCategory::Engine, "User settings saved to {}", path.string());
	}
	const SettingInfo& SettingMetadata(const std::string_view key)
	{
		for (const auto& [candidate, info]: kSettingInfo)
		{
			if (candidate == key)
			{
				return info;
			}
		}
		// A key with no entry is not an error - it renders as a plain field with no help.
		static const SettingInfo kNone{};
		return kNone;
	}

} // namespace aether
