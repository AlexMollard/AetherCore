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
#include "utils/Profiler.hpp"
#include "utils/TextIni.hpp"

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
		        {"window.mode", {.description = "Borderless is the only mode besides fullscreen that can win DWM independent flip; composition costs about a frame of latency.", .choices = kWindowModes, .restartRequired = true}},
		        {"graphics.vsync", {.description = "Wait for the display to refresh. Turning it off tears, but removes a frame of latency."}},
		        {"graphics.framesInFlight", {.description = "How far the game thread may run ahead of the screen. Every frame of run-ahead is one display interval of input lag (~17 ms at 60 Hz).", .minValue = 1.0, .maxValue = 3.0}},
		        {"graphics.lowLatencyPresent", {.description = "Prefer MAILBOX over FIFO while vsync is on: a finished frame replaces the pending one instead of queueing behind it."}},
		        {"graphics.latencyPacing", {.description = "Idle out most of the display interval and latch input just before the flip. Needs a measured flip phase; does nothing without one."}},
		        {"graphics.renderScale", {.description = "Render the scene at this fraction of the output and upscale it. UI still draws at native resolution.", .minValue = 0.25, .maxValue = 1.0, .restartRequired = true}},
		        {"graphics.fxaa", {.description = "Cheap post-process antialiasing."}},
		        {"graphics.gtao", {.description = "Ground-truth ambient occlusion: darkens creases and contact points that the light probes cannot see into."}},
		        {"graphics.gtaoRadius", {.description = "How far, in world units, a surface looks for geometry occluding it. Too small and only tight creases darken; too large and the whole scene greys.", .minValue = 0.1, .maxValue = 5.0}},
		        {"graphics.gtaoStrength", {.description = "How hard the occlusion is applied. Affects ambient light only, never direct light.", .minValue = 0.0, .maxValue = 3.0}},
		        {"graphics.reflections", {.description = "Screen-space reflections: marches the depth buffer so surfaces reflect what is actually in front of them instead of only the sky."}},
		        {"graphics.reflectionMaxRoughness", {.description = "Roughness above which reflections fall back to the sky probe. A wide lobe needs many rays to look like anything but noise.", .minValue = 0.0, .maxValue = 1.0}},
		        {"graphics.reflectionIntensity", {.description = "Strength of the screen-space reflection contribution.", .minValue = 0.0, .maxValue = 2.0}},
		        {"graphics.contactShadows", {.description = "Recovers the small shadow at the base of an object that a shadow cascade texel is too coarse to resolve, by marching a short ray through the depth buffer."}},
		        {"graphics.anisotropy", {.description = "Texture samples taken along the footprint when a surface is seen edge-on. 1 disables it, which visibly blurs ground at a grazing angle.", .minValue = 1.0, .maxValue = 16.0, .restartRequired = true}},
		        {"graphics.asyncCompute", {.description = "Overlap compute work with graphics on a separate queue.", .restartRequired = true}},
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
		settings.graphics.renderScale = std::clamp(settings.graphics.renderScale, 0.25f, 1.0f);

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
			        current.push_back({section, name, FormatField(field), IsProjectOnlySettingKey(key)});
		        });
		ForEachSettingField(base, [&](std::string_view, const auto& field) { baseline.push_back(FormatField(field)); });

		std::ostringstream out;
		out << "# AetherCore user settings (TOML)\n";
		out << "# Overrides layered on top of the shipped EngineSettings.toml; only changed keys are stored.\n";

		std::string currentSection;
		for (std::size_t i = 0; i < current.size(); ++i)
		{
			// Project-only keys are never a user preference, so they never enter the
			// per-user file even when they differ from the base.
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
