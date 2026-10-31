#pragma once

#include <span>

#include "editor/EditorProjectActions.hpp"
#include "editor/publish/PublishSteps.hpp"

namespace aether::editor
{
	// Runs the native pipeline in an isolated sibling directory. The last good output
	// stays in place until all steps succeed; a failed promotion restores it.
	[[nodiscard]] StepResult RunNativePublishSteps(const PublishPlan& plan, std::span<const PublishStep> steps, PublishContext& context,
	        const PublishToolchain& toolchain, const EditorProjectPublishProgress& progress = {});
}
