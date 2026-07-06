#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aether
{
	// A named editor layout: the ImGui .ini blob (dock geometry + per-window
	// placement) plus each panel's open/closed state. Persisted one file per
	// preset under the user config dir so layouts survive restarts.
	struct LayoutPreset
	{
		std::string name;
		std::vector<std::pair<std::string, bool>> visibility;
		std::string imguiIni;
	};

	// Load/save/delete for named layout presets. The (de)serialization is pure
	// text - the ImGui ini is stored verbatim as the file tail (no escaping), so
	// its '[Section]' headers, '=' pairs and newlines survive a round-trip.
	class LayoutPresetStore
	{
	public:
		// Directory presets live in: GetUserConfigDir()/"layouts". Empty if no
		// user config dir is available. Created on demand by Save.
		[[nodiscard]] static std::filesystem::path DefaultDir();

		[[nodiscard]] static std::vector<LayoutPreset> LoadAll();
		[[nodiscard]] static std::vector<LayoutPreset> LoadAll(const std::filesystem::path& dir);

		static bool Save(const LayoutPreset& preset);
		static bool Save(const LayoutPreset& preset, const std::filesystem::path& dir);

		static bool Remove(std::string_view name);
		static bool Remove(std::string_view name, const std::filesystem::path& dir);

		// Pure text (de)serialization - the unit-tested core.
		[[nodiscard]] static std::string Serialize(const LayoutPreset& preset);
		[[nodiscard]] static std::optional<LayoutPreset> Deserialize(std::string_view text);

		// Filesystem-safe filename stem for a preset name (never empty).
		[[nodiscard]] static std::string SlugFor(std::string_view name);
	};
} // namespace aether
