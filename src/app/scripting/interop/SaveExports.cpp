#include "scripting/interop/InteropCommon.hpp"

#include <cstring>
#include <limits>

#include "scripting/GameplaySaveStore.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// Positive result: required byte capacity including a trailing NUL. No partial
// copy on short buffers. -1 means missing/read-isolated; zero means failure.
AE_SCRIPT_API std::int32_t aether_save_read(const char* key, char* output, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
	{
		if (key == nullptr || capacity < 0 || (capacity != 0 && output == nullptr)) { return 0; }
		std::string value;
		const auto status = GameplaySaveStore(GameplaySaveStore::ProjectDirectory()).Read(key, value, ActiveContext().allowSaveReads);
		if (status == AE_GAMEPLAY_INVALID_HANDLE) { return -1; }
		if (status != AE_GAMEPLAY_OK || value.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) { return 0; }
		const auto required = static_cast<std::int32_t>(value.size() + 1);
		if (capacity >= required)
		{
			std::memcpy(output, value.data(), value.size());
			output[value.size()] = '\0';
		}
		return required;
	});
}

AE_SCRIPT_API std::int32_t aether_save_write(const char* key, const char* value, std::int32_t length)
{
	return SafeExport([&] -> std::int32_t
	{
		if (key == nullptr || length < 0 || (length != 0 && value == nullptr)) { return 0; }
		const auto text = length == 0 ? std::string_view{} : std::string_view(value, static_cast<std::size_t>(length));
		return GameplaySaveStore(GameplaySaveStore::ProjectDirectory()).Write(key, text) == AE_GAMEPLAY_OK ? 1 : 0;
	});
}
