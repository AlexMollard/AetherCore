#pragma once

#include <filesystem>

namespace aether::app
{
	// The three inputs that decide which project file a game runtime layers over its own
	// EngineSettings.toml. Split out from the environment so the rule can be tested - it is
	// otherwise invisible until a published build boots the wrong scene on a dev machine.
	//
	// Lives outside project/ on purpose: that directory is editor tooling and is filtered
	// out of the shipped runtime, but this rule is the runtime's own.
	struct RuntimeProjectSettingsInputs
	{
		std::filesystem::path envProjectDir;      // AETHER_PROJECT_DIR; empty when unset
		std::filesystem::path exeDir;             // where the runtime executable lives
		std::filesystem::path compiledDefaultDir; // AETHER_DEFAULT_PROJECT_DIR; empty if not compiled in
	};

	// Empty means "no project layer" - the settings beside the executable are already the
	// whole truth. A published package must land here: it ships data/project.pak and had
	// its project settings baked in at publish time, so reaching out to the project
	// directory compiled into the binary would let the BUILD machine's startup scene
	// replace the published one.
	[[nodiscard]] std::filesystem::path ResolveRuntimeProjectSettings(const RuntimeProjectSettingsInputs& inputs);
} // namespace aether::app
