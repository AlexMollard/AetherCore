#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "scripting/GameplayServices.h"

namespace aether::app::scripting
{
	// Both language adapters use the same engine-owned key namespace and read policy.
	class GameplaySaveStore
	{
	public:
		explicit GameplaySaveStore(std::filesystem::path directory);
		[[nodiscard]] static std::filesystem::path ProjectDirectory();
		[[nodiscard]] AetherGameplayStatus Read(std::string_view key, std::string& output, bool allowReads) const;
		[[nodiscard]] AetherGameplayStatus Write(std::string_view key, std::string_view value) const;

	private:
		std::filesystem::path m_directory;
	};
}
