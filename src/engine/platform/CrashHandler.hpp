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
} // namespace aether
