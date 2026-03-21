#pragma once

#include <string_view>

namespace meow
{
	class CrashHandler
	{
	public:
		static void Install(std::string_view appName = "MeowCore");
		static void Uninstall();
		static void ReportGraphicsFault(std::string_view stage, std::string_view detail);
	};
}
