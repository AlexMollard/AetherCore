#pragma once

#include <filesystem>

namespace aether::app::launcher
{
	// Spawn the Editor process for `projectRoot` as a detached child:
	//   "<launcher-exe-dir>/<AETHER_EDITOR_EXE_NAME> --project <projectRoot>"
	// The launcher keeps running (Hub-style). A separate process gives the editor a
	// real separate OS window. Returns true if the process started. Windows-only for
	// now (the dev target); other platforms log and return false.
	bool SpawnEditor(const std::filesystem::path& projectRoot);
} // namespace aether::app::launcher
