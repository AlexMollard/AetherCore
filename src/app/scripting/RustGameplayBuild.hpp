#pragma once

#include <filesystem>
#include "scripting/AgentDiagnostics.hpp"

namespace aether::app::scripting
{
	struct RustBuildRequest
	{
		std::filesystem::path manifest;
		std::filesystem::path artifactsDir;
		std::filesystem::path outputDir;
		bool release = false;
		int timeoutMs = 180000;
	};
	struct RustBuildResult
	{
		aether::scripting::AgentReport report;
		std::filesystem::path module;
		[[nodiscard]] bool Succeeded() const { return report.status == aether::scripting::AgentJobStatus::Passed; }
	};
	[[nodiscard]] RustBuildResult BuildRustGameplay(const RustBuildRequest& request);
}
