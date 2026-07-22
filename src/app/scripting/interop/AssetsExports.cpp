// Generic managed asset-text read. Runtime-safe: engine io only, no editor/ImGui deps.
// Reads a VFS virtual path as UTF-8 text (raw project dir in-editor, pak when shipped).

#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>

#include "io/FileSystem.hpp"

// Fills `out` with up to `cap` bytes of the asset text. Returns the FULL byte length
// (may exceed cap -> caller grows and retries). Returns -1 if the asset is missing/unreadable.
AE_SCRIPT_API std::int32_t aether_assets_read_text(const char* vpath, char* out, std::int32_t cap)
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
}
