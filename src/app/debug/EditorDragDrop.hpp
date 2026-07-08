#pragma once

#include <cstdint>

namespace aether::app::dragdrop
{
	inline constexpr const char* kEntityPayload = "AETHER_ENTITY";
	inline constexpr const char* kScriptPayload = "AETHER_SCRIPT_TYPE";
	inline constexpr const char* kFilePayload = "AETHER_FILE_PATH";

	struct ScriptPayload
	{
		char typeName[128] = {};
		char sourcePath[260] = {};
	};

	struct FilePayload
	{
		char path[260] = {};
	};

	struct EntityPayload
	{
		std::uint32_t id = 0;
	};
} // namespace aether::app::dragdrop
