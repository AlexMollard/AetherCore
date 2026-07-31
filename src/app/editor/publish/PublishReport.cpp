#include "editor/publish/PublishReport.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "editor/publish/PublishPlan.hpp"
#include "io/FileUtil.hpp"
#include "utils/EngineSettings.hpp"

namespace aether::editor
{
	namespace
	{
		std::string HumanBytes(const std::uintmax_t bytes)
		{
			constexpr double kKib = 1024.0;
			const auto value = static_cast<double>(bytes);
			if (value < kKib)
			{
				return std::to_string(bytes) + " B";
			}
			std::ostringstream out;
			out.setf(std::ios::fixed);
			out.precision(2);
			if (value < kKib * kKib)
			{
				out << value / kKib << " KB";
			}
			else
			{
				out << value / (kKib * kKib) << " MB";
			}
			return out.str();
		}
	} // namespace

	PublishReport BuildPublishReport(const PublishPlan& plan)
	{
		PublishReport report;

		std::vector<std::pair<std::string, std::uintmax_t>> files;
		std::uintmax_t total = 0;
		std::error_code ec;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(plan.outputDir, ec))
		{
			if (ec || !entry.is_regular_file(ec))
			{
				continue;
			}
			const std::uintmax_t size = std::filesystem::file_size(entry.path(), ec);
			if (ec)
			{
				continue;
			}
			const std::filesystem::path rel = std::filesystem::relative(entry.path(), plan.outputDir, ec);
			files.emplace_back(ec ? entry.path().filename().generic_string() : rel.generic_string(), size);
			total += size;
		}

		// Read what actually shipped, not what the project says - the report describes the
		// package, and the two can only differ if something went wrong.
		std::string startupScene = "(unset)";
		bool autoplay = false;
		if (auto settingsText = io::file_util::ReadText(plan.outputDir / "data" / "config" / "EngineSettings.toml"))
		{
			EngineSettings settings{};
			EngineSettingsIO::Apply(*settingsText, settings);
			if (!settings.app.startupScene.empty())
			{
				startupScene = settings.app.startupScene;
			}
			autoplay = settings.app.autoplay;
		}

		const auto sizeOf = [&files](const std::string_view rel) -> std::uintmax_t
		{
			for (const auto& [path, size]: files)
			{
				if (path == rel)
				{
					return size;
				}
			}
			return 0;
		};

		std::vector<std::pair<std::string, std::uintmax_t>> largest = files;
		std::ranges::sort(largest, [](const auto& a, const auto& b) { return a.second > b.second; });

		std::string text;
		text += "AetherCore Publish Report\n";
		text += "=========================\n\n";
		text += "Product:       " + plan.productName + "\n";
		text += "Platform:      " + plan.platformName + "\n";
		text += "Runtime:       " + plan.runtimeExeName + "\n";
		text += "Configuration: " + plan.configName + (plan.shippableConfig ? "\n" : "  (testing only - not a shippable build)\n");
		text += "Startup scene: " + startupScene + "  (autoplay: " + (autoplay ? "yes" : "no") + ")\n\n";
		text += "Package\n-------\n";
		text += "Total size:    " + HumanBytes(total) + "\n";
		text += "Files:         " + std::to_string(files.size()) + "\n\n";
		text += "Key payload\n-----------\n";
		text += "engine.pak     " + HumanBytes(sizeOf("data/engine.pak")) + "\n";
		text += "project.pak    " + HumanBytes(sizeOf("data/project.pak")) + "\n";
		text += "runtime exe    " + HumanBytes(sizeOf(plan.runtimeExeName)) + "\n\n";
		text += "Largest files\n-------------\n";
		for (std::size_t i = 0; i < largest.size() && i < 8; ++i)
		{
			char line[512];
			std::snprintf(line, sizeof(line), "  %12s  %s\n", HumanBytes(largest[i].second).c_str(), largest[i].first.c_str());
			text += line;
		}

		report.text = std::move(text);
		report.summary = "Published " + plan.productName + " (" + HumanBytes(total) + ", " + std::to_string(files.size()) + " files, " + plan.configName + ", startup '" + startupScene + "').";
		report.ok = !files.empty();
		return report;
	}
} // namespace aether::editor
