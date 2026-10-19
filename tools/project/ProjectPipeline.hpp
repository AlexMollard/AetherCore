#pragma once

#include <filesystem>
#include <functional>
#include <stop_token>
#include <nlohmann/json.hpp>

namespace aether::project_tool
{
	using Progress = std::function<void(const nlohmann::json&)>;
	[[nodiscard]] nlohmann::json ExecutePipeline(nlohmann::json request, const std::filesystem::path& directory, std::stop_token stop, const Progress& progress);
	[[nodiscard]] nlohmann::json ExecuteChild(const nlohmann::json& request, bool publish);
}
