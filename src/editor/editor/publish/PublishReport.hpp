#pragma once

#include <string>

#include "scripting/AgentDiagnostics.hpp"

namespace aether::editor
{
	struct PublishPlan;

	// Describes what actually shipped. Written next to the package as publish-report.txt so
	// a build can be identified after it leaves the machine that made it.
	struct PublishReport
	{
		bool ok = false;
		std::string summary; // one line, shown in the Build panel
		std::string text;    // full report file contents
	};

	[[nodiscard]] PublishReport BuildPublishReport(const PublishPlan& plan);

	// Adapts a status snapshot without altering the legacy report or inferring failure
	// from its default false `ok` while publishing is still in progress.
	[[nodiscard]] Expected<scripting::AgentReport> BuildPublishAgentReport(const PublishReport& report, scripting::AgentReport context, bool completed);
} // namespace aether::editor
