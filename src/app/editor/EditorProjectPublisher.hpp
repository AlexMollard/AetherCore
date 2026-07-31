#pragma once

#include "editor/EditorProjectActions.hpp"
#include "editor/publish/PublishPlan.hpp"

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	// The plan a publish would use. The Build panel calls this to show the destination and
	// the build configuration before anything runs.
	[[nodiscard]] PublishPlan PlanPublish(const app::EditorProjectContext& project);

	// Bakes the project into Builds/Pack/project.pak and refreshes the editor's own copy so
	// the dev runtime can load it. This is the dev-loop action, not a shippable package.
	[[nodiscard]] EditorProjectActionResult PackProject(const app::EditorProjectContext& project);

	// One action, no options: everything it needs is derived from the project and the
	// running editor. Runs every step in PublishStepList and verifies the result.
	[[nodiscard]] EditorProjectActionResult PublishProject(const app::EditorProjectContext& project, const EditorProjectPublishProgress& progress = {});
} // namespace aether::editor
