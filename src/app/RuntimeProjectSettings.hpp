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

	// Which assembly the runtime is about to treat as "the game's scripts".
	struct RuntimeScriptAssemblyInputs
	{
		std::filesystem::path envProjectDir; // AETHER_PROJECT_DIR; empty when unset
		std::filesystem::path exeDir;        // where the runtime executable lives
		std::filesystem::path managedDir;    // where AetherGame.dll is actually loaded from
	};

	// True when the runtime was pointed at a specific project but the assembly it loads
	// cannot be that project's.
	//
	// The runtime stages one AetherGame.dll beside itself and loads it unconditionally,
	// which is right for a published package - that package has exactly one project and the
	// staged assembly is its own. It is wrong the moment someone points a DEV build tree at
	// a project with --project: the settings and scenes come from that project while the
	// scripts come from whatever the engine staged, so the game boots the correct world
	// running none of its own code.
	//
	// That failure is completely silent and looks like a healthy start - correct window,
	// correct scene, right entity count - which is exactly why it needs saying out loud. It
	// cost a full debugging session: menus that answered neither mouse, keyboard nor
	// gamepad, because the scripts that would have answered were never in the assembly.
	[[nodiscard]] bool ProjectScriptsCannotBeLoaded(const RuntimeScriptAssemblyInputs& inputs);
} // namespace aether::app
