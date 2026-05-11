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
	};
} // namespace aether
