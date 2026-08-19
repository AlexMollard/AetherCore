#pragma once

#include <string_view>

namespace aether
{
	class CrashHandler
	{
	public:
		static void Install(std::string_view appName = "AetherCore");
		static void Uninstall();
		static void ReportGraphicsFault(std::string_view stage, std::string_view detail);

		// Thread-safe; a repeated key overwrites. Any subsystem can enrich the
		static void SetContext(std::string_view key, std::string_view value);
	};

	// Whether a debugger is attached RIGHT NOW - queried per call, not cached, so
	// attaching to an already-running process is picked up too.
	//
	// Used to keep the editor from handing off to another process while someone is
	// stepping through it: a spawn is a new, undebugged process and the session dies with
	// the one that spawned it. Never change what the engine computes on this - only how a
	// developer-facing hand-off behaves.
	[[nodiscard]] bool IsDebuggerAttached();
} // namespace aether
