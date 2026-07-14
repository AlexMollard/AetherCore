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
	struct LayoutPreset
	{
		std::string name;
		std::vector<std::pair<std::string, bool>> visibility;
		std::string imguiIni;
	};

	// Load/save/delete for named layout presets. The (de)serialization is pure
	class LayoutPresetStore
	{
	public:
		[[nodiscard]] static std::filesystem::path DefaultDir();

		[[nodiscard]] static std::vector<LayoutPreset> LoadAll();
		[[nodiscard]] static std::vector<LayoutPreset> LoadAll(const std::filesystem::path& dir);

		static bool Save(const LayoutPreset& preset);
		static bool Save(const LayoutPreset& preset, const std::filesystem::path& dir);

		static bool Remove(std::string_view name);
		static bool Remove(std::string_view name, const std::filesystem::path& dir);

		[[nodiscard]] static std::string Serialize(const LayoutPreset& preset);
		[[nodiscard]] static std::optional<LayoutPreset> Deserialize(std::string_view text);

		// Filesystem-safe filename stem for a preset name (never empty).
		[[nodiscard]] static std::string SlugFor(std::string_view name);
	};
} // namespace aether
