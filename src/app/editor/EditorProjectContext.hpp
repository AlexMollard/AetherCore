#pragma once

#include <filesystem>
#include <string>

namespace aether::app
{
	struct EditorProjectContext
	{
		std::filesystem::path root;
		std::filesystem::path projectFile;
		std::filesystem::path assetsDir;
		std::filesystem::path scenesDir;
		std::filesystem::path prefabsDir;
		std::filesystem::path scriptsDir;
		std::string name;
		bool loaded = false;

		[[nodiscard]] bool IsLoaded() const noexcept
		{
			return loaded && !root.empty();
		}
	};
} // namespace aether::app
