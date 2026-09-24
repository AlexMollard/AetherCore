#pragma once

#include <string>

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
} // namespace aether::editor
