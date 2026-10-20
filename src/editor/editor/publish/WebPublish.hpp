#pragma once

#include <filesystem>
#include <stop_token>
#include <nlohmann/json.hpp>

namespace aether::editor
{
	struct PublishPlan;
	nlohmann::json RunWebPublish(const PublishPlan& plan, nlohmann::json request, const std::filesystem::path& directory, std::stop_token stop = {});
} // namespace aether::editor
