// Generic managed asset-text read. Runtime-safe: engine io only, no editor/ImGui deps.
// Reads a VFS virtual path as UTF-8 text (raw project dir in-editor, pak when shipped).

#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "io/FileSystem.hpp"

using namespace aether::app::scripting::interop;

// Fills `out` with up to `cap` bytes of the asset text. Returns the FULL byte length
// (may exceed cap -> caller grows and retries). Returns -1 if the asset is missing/unreadable.
AE_SCRIPT_API std::int32_t aether_assets_read_text(const char* vpath, char* out, std::int32_t cap)
{
	return SafeExport([&] -> std::int32_t
	{
	if (vpath == nullptr)
	{
		return -1;
	}
	auto text = aether::io::FileSystem::ReadFileText(vpath);
	if (!text)
	{
		return -1;
	}
	const std::int32_t full = static_cast<std::int32_t>(text->size());
	if (out != nullptr && cap > 0)
	{
		const std::int32_t n = std::min<std::int32_t>(cap, full);
		std::memcpy(out, text->data(), static_cast<std::size_t>(n));
	}
	return full;
	});
}

// Writes UTF-8 text to a VFS path (the loose project dir in-editor). Returns 1 on success, 0 on
// failure (e.g. a read-only shipped pak). Editor-tooling write, symmetric with the read above.
AE_SCRIPT_API std::int32_t aether_assets_write_text(const char* vpath, const char* text)
{
	return SafeExport([&] -> std::int32_t
	{
	if (vpath == nullptr)
	{
		return 0;
	}
	const auto result = aether::io::FileSystem::WriteFileText(vpath, text != nullptr ? text : "");
	return result ? 1 : 0;
	});
}

// Globs a VFS pattern (e.g. "project://assets/dialogue/*.json") and returns the matches as a
// '\n'-joined UTF-8 string (full length returned; caller grows + retries). -1 on error.
AE_SCRIPT_API std::int32_t aether_assets_list(const char* pattern, char* out, std::int32_t cap)
{
	return SafeExport([&] -> std::int32_t
	{
	if (pattern == nullptr)
	{
		return -1;
	}
	auto matches = aether::io::FileSystem::Glob(pattern);
	if (!matches)
	{
		return -1;
	}
	std::string joined;
	for (const std::string& m : *matches)
	{
		if (!joined.empty())
		{
			joined.push_back('\n');
		}
		joined += m;
	}
	const std::int32_t full = static_cast<std::int32_t>(joined.size());
	if (out != nullptr && cap > 0)
	{
		const std::int32_t n = std::min<std::int32_t>(cap, full);
		std::memcpy(out, joined.data(), static_cast<std::size_t>(n));
	}
	return full;
	});
}
