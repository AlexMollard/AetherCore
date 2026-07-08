#include "utils/LayoutPresetStore.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

#include "io/PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		std::string_view Trim(std::string_view s)
		{
			const auto notSpace = [](char c)
			{
				return c != ' ' && c != '\t';
			};
			while (!s.empty() && !notSpace(s.front()))
			{
				s.remove_prefix(1);
			}
			while (!s.empty() && !notSpace(s.back()))
			{
				s.remove_suffix(1);
			}
			return s;
		}

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
		out += preset.imguiIni; // verbatim tail
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
				// Everything after this line is the ImGui ini, stored verbatim.
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
				const std::string_view key = Trim(line.substr(0, eq));
				const std::string_view value = Trim(line.substr(eq + 1));
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
			return std::nullopt; // malformed: no ini section
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
			// other characters are dropped
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
		std::error_code ec;
		if (!std::filesystem::exists(dir, ec))
		{
			return result;
		}
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
			std::ifstream in(entry.path(), std::ios::binary);
			if (!in.is_open())
			{
				continue;
			}
			const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
			if (auto preset = Deserialize(text))
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
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const auto path = dir / (SlugFor(preset.name) + ".layout");
		// Binary mode: keep the ini's '\n' unmangled so a save/load round-trips
		// exactly and matches ImGui's own newline convention.
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			AE_WARN(LogCategory::Engine, "Failed to write layout preset: {}", path.string());
			return false;
		}
		out << Serialize(preset);
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
		std::error_code ec;
		return std::filesystem::remove(dir / (SlugFor(name) + ".layout"), ec);
	}
} // namespace aether
