#pragma once

#include <filesystem>

namespace aether::app::launcher
{
	// Spawn the Editor process for `projectRoot` as a detached child:
	//   "<launcher-exe-dir>/<AETHER_EDITOR_EXE_NAME> --project <projectRoot>"
	// The launcher keeps running (Hub-style). A separate process gives the editor a
	// real separate OS window. Returns true if the process started. Windows-only for
	// now (the dev target); other platforms log and return false.
	//
	// controlPort > 0 forwards AETHER_CONTROL_PORT=<controlPort> to the spawned editor
	// so its ControlServer auto-starts on that port and the AetherCore MCP / aether-ctl
	// can drive it (see tools/mcp/). controlPort <= 0 leaves the child's environment
	// untouched (the editor starts no control endpoint unless the env already sets one).
	bool SpawnEditor(const std::filesystem::path& projectRoot, int controlPort = 0);
} // namespace aether::app::launcher
