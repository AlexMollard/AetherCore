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

		// Register a key/value pair that is written into every subsequent crash
		// report's header (e.g. GPU adapter + driver, current scene, build id).
		// Thread-safe; a repeated key overwrites. Any subsystem can enrich the
		// report without the crash handler needing to know about it - so a crash
		// dump can say "which GPU / which scene / which build" without spelunking
		// the log tail. Setting an empty value removes the key.
		static void SetContext(std::string_view key, std::string_view value);
	};
} // namespace aether
