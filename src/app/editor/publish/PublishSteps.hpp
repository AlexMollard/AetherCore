#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace aether::editor
{
	struct PublishPlan;

	// Where the external tools live. Populated from the AETHER_* compile definitions.
	struct PublishToolchain
	{
		std::filesystem::path dotnetExe;
		std::string managedConfig;
		std::string managedConfigDir;
		std::filesystem::path managedSdkProject;
	};

	[[nodiscard]] PublishToolchain MakePublishToolchain();

	// Mutable state threaded through the steps: what earlier steps produced.
	struct PublishContext
	{
		std::filesystem::path packedProjectPak;
		std::string reportSummary;
	};

	struct StepResult
	{
		bool ok = true;
		std::string message;
		std::string remediation;
	};

	struct PublishStep
	{
		std::string_view name; // shown as the progress stage
		StepResult (*run)(const PublishPlan&, PublishContext&, const PublishToolchain&);
	};

	// The pipeline, in order. Progress is derived from position in this list, so adding a
	// step never requires retuning a progress fraction.
	[[nodiscard]] std::span<const PublishStep> PublishStepList();
} // namespace aether::editor
