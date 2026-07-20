#include "utils/LayoutPresetStore.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

namespace aether
{
	namespace
	{
		constexpr std::string_view kImguiMarker = "[imgui]";
		constexpr std::string_view kVisibilityMarker = "[visibility]";
	} // namespace

	std::string LayoutPresetStore::Serialize(const LayoutPreset& preset)
	{
		std::string out;
		out += "name = ";
		out += preset.name;
		out += "\n";
		out += kVisibilityMarker;
		out += "\n";
		for (const auto& [panel, visible]: preset.visibility)
		{
			out += panel;
			out += " = ";
			out += visible ? "1" : "0";
			out += "\n";
		}
		out += kImguiMarker;
		out += "\n";
		out += preset.imguiIni;
		return out;
	}

	std::optional<LayoutPreset> LayoutPresetStore::Deserialize(std::string_view text)
	{
		LayoutPreset preset;
		bool inVisibility = false;
		bool sawImgui = false;

		std::size_t pos = 0;
		while (pos < text.size())
		{
			const std::size_t nl = text.find('\n', pos);
			const std::size_t lineEnd = (nl == std::string_view::npos) ? text.size() : nl;
			const std::size_t nextPos = (nl == std::string_view::npos) ? text.size() : nl + 1;

			std::string_view line = text.substr(pos, lineEnd - pos);
			if (!line.empty() && line.back() == '\r')
			{
				line.remove_suffix(1);
			}

			if (line == kImguiMarker)
			{
				preset.imguiIni.assign(text.substr(nextPos));
				sawImgui = true;
				break;
			}
			if (line == kVisibilityMarker)
			{
				inVisibility = true;
				pos = nextPos;
				continue;
			}

			if (const std::size_t eq = line.find('='); eq != std::string_view::npos)
			{
				const std::string_view key = utils::TrimView(line.substr(0, eq), " \t");
				const std::string_view value = utils::TrimView(line.substr(eq + 1), " \t");
				if (inVisibility)
				{
					preset.visibility.emplace_back(std::string(key), value == "1" || value == "true");
				}
				else if (key == "name")
				{
					preset.name.assign(value);
				}
			}
			pos = nextPos;
		}

		if (!sawImgui)
		{
			return std::nullopt;
		}
		return preset;
	}

	std::string LayoutPresetStore::SlugFor(std::string_view name)
	{
		std::string slug;
		slug.reserve(name.size());
		for (const char c: name)
		{
			if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_')
			{
				slug += c;
			}
			else if (c == ' ')
			{
				slug += '_';
			}
		}
		if (slug.empty())
		{
			slug = "layout";
		}
		return slug;
	}

	std::filesystem::path LayoutPresetStore::DefaultDir()
	{
		const auto base = io::PlatformPaths::GetUserConfigDir();
		if (base.empty())
		{
			return {};
		}
		return base / "layouts";
	}

	std::vector<LayoutPreset> LayoutPresetStore::LoadAll()
	{
		const auto dir = DefaultDir();
		if (dir.empty())
		{
			return {};
		}
		return LoadAll(dir);
	}

	std::vector<LayoutPreset> LayoutPresetStore::LoadAll(const std::filesystem::path& dir)
	{
		std::vector<LayoutPreset> result;
		if (!io::file_util::Exists(dir))
		{
			return result;
		}
		std::error_code ec;
		for (const auto& entry: std::filesystem::directory_iterator(dir, ec))
		{
			if (ec)
			{
				break;
			}
			if (!entry.is_regular_file() || entry.path().extension() != ".layout")
			{
				continue;
			}
			auto text = io::file_util::ReadText(entry.path());
			if (!text)
			{
				continue;
			}
			if (auto preset = Deserialize(*text))
			{
				result.push_back(std::move(*preset));
			}
		}
		std::sort(result.begin(), result.end(), [](const LayoutPreset& a, const LayoutPreset& b) { return a.name < b.name; });
		return result;
	}

	bool LayoutPresetStore::Save(const LayoutPreset& preset)
	{
		const auto dir = DefaultDir();
		if (dir.empty())
		{
			AE_WARN(LogCategory::Engine, "No user config directory available; layout preset not saved.");
			return false;
		}
		return Save(preset, dir);
	}

	bool LayoutPresetStore::Save(const LayoutPreset& preset, const std::filesystem::path& dir)
	{
		const auto path = dir / (SlugFor(preset.name) + ".layout");
		if (auto result = io::file_util::WriteText(path, Serialize(preset)); !result)
		{
			AE_WARN(LogCategory::Engine, "Failed to write layout preset: {} - {}", path.string(), result.error().message);
			return false;
		}
		return true;
	}

	bool LayoutPresetStore::Remove(std::string_view name)
	{
		const auto dir = DefaultDir();
		if (dir.empty())
		{
			return false;
		}
		return Remove(name, dir);
	}

	bool LayoutPresetStore::Remove(std::string_view name, const std::filesystem::path& dir)
	{
		return io::file_util::Remove(dir / (SlugFor(name) + ".layout")).has_value();
	}
} // namespace aether
