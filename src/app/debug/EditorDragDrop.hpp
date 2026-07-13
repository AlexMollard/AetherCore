#pragma once

#include <cstdint>

namespace aether::editor::dragdrop
{
	inline constexpr const char* kEntityPayload = "AETHER_ENTITY";
	inline constexpr const char* kScriptPayload = "AETHER_SCRIPT_TYPE";
	inline constexpr const char* kFilePayload = "AETHER_FILE_PATH";

	enum class FileKind : std::uint32_t
	{
		Unknown,
		Model,
		Material,
		Texture,
		Script,
		Prefab,
		Scene,
	};

	struct ScriptPayload
	{
		char typeName[128] = {};
		char sourcePath[260] = {};
	};

	struct FilePayload
	{
		FileKind kind = FileKind::Unknown;
		char path[260] = {};
		char displayName[128] = {};
	};

	struct EntityPayload
	{
		std::uint32_t id = 0;
	};
} // namespace aether::editor::dragdrop
