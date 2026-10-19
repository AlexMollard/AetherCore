#include "GameplayAbiValidation.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace aether::scripting
{
	static_assert(sizeof(AetherGameplayEntity) == 16);
	static_assert(sizeof(AetherGameplayPropertyValue) == 48);
	static_assert(std::is_standard_layout_v<AetherGameplayModuleApi>);
	static_assert(std::is_trivially_copyable_v<AetherGameplayPropertyValue>);

	namespace
	{
		AetherGameplayStatus PrepareOutput(uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi) noexcept
		{
			if (outApi == nullptr)
			{
				return AE_GAMEPLAY_INVALID_ARGUMENT;
			}
			std::memset(outApi, 0, std::min<size_t>(outSize, sizeof(*outApi)));
			if (requestedMajor != AE_GAMEPLAY_ABI_MAJOR)
			{
				return AE_GAMEPLAY_ABI_MISMATCH;
			}
			if (outSize < sizeof(*outApi))
			{
				return AE_GAMEPLAY_BUFFER_TOO_SMALL;
			}
			return outSize == sizeof(*outApi) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_INVALID_ARGUMENT;
		}
	} // namespace

	AetherGameplayStatus ValidateGameplayModule(const AetherGameplayModuleApi& api) noexcept
	{
		if (api.abiMajor != AE_GAMEPLAY_ABI_MAJOR || api.structSize != sizeof(api))
		{
			return AE_GAMEPLAY_ABI_MISMATCH;
		}
		if (api.initialize == nullptr || api.shutdown == nullptr || api.getTypeCount == nullptr || api.getTypeInfo == nullptr || api.getPropertyInfo == nullptr || api.create == nullptr || api.destroy == nullptr || api.attach == nullptr
		        || api.update == nullptr || api.detach == nullptr || api.getProperty == nullptr || api.setProperty == nullptr || api.getDefaultProperty == nullptr)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		constexpr uint64_t kEventCapabilities = AE_GAMEPLAY_CAP_PHYSICS | AE_GAMEPLAY_CAP_OWNERSHIP | AE_GAMEPLAY_CAP_NETWORK;
		if ((api.capabilities & ~kEventCapabilities) != 0 || ((api.capabilities & kEventCapabilities) != 0 && api.dispatchEvent == nullptr))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus ValidateGameplayEngine(const AetherGameplayEngineApi& api) noexcept
	{
		if (api.abiMajor != AE_GAMEPLAY_ABI_MAJOR || api.structSize != sizeof(api))
		{
			return AE_GAMEPLAY_ABI_MISMATCH;
		}
		return api.context != 0 && api.entityValid != nullptr && api.getPosition != nullptr && api.setPosition != nullptr && api.reportDiagnostic != nullptr ? AE_GAMEPLAY_OK : AE_GAMEPLAY_INVALID_ARGUMENT;
	}

	AetherGameplayStatus PublishGameplayModule(const AetherGameplayModuleApi& source, uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi) noexcept
	{
		// Copy before clearing in case source aliases the destination.
		const auto copy = source;
		const auto outputStatus = PrepareOutput(requestedMajor, outSize, outApi);
		if (outputStatus != AE_GAMEPLAY_OK)
		{
			return outputStatus;
		}
		const auto validationStatus = ValidateGameplayModule(copy);
		if (validationStatus == AE_GAMEPLAY_OK)
		{
			*outApi = copy;
		}
		return validationStatus;
	}

	AetherGameplayStatus QueryGameplayModule(AetherGameplayQuery query, uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi) noexcept
	{
		const auto outputStatus = PrepareOutput(requestedMajor, outSize, outApi);
		if (outputStatus != AE_GAMEPLAY_OK)
		{
			return outputStatus;
		}
		if (query == nullptr)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		AetherGameplayModuleApi candidate{};
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		try
#endif
		{
			const auto status = query(requestedMajor, sizeof(candidate), &candidate);
			if (status != AE_GAMEPLAY_OK)
			{
				return status;
			}
		}
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		catch (...)
		{
			return AE_GAMEPLAY_SCRIPT_ERROR;
		}
#endif
		return PublishGameplayModule(candidate, requestedMajor, outSize, outApi);
	}

	bool IsGameplayUtf8(AetherGameplayUtf8 value) noexcept
	{
		if (value.data == nullptr)
		{
			return value.length == 0;
		}
		const auto* bytes = reinterpret_cast<const unsigned char*>(value.data);
		for (uint32_t index = 0; index < value.length;)
		{
			const uint32_t lead = bytes[index++];
			if (lead < 0x80)
			{
				continue;
			}
			uint32_t continuationCount;
			uint32_t codePoint;
			uint32_t minimum;
			if (lead >= 0xC2 && lead <= 0xDF)
			{
				continuationCount = 1;
				codePoint = lead & 0x1F;
				minimum = 0x80;
			}
			else if (lead >= 0xE0 && lead <= 0xEF)
			{
				continuationCount = 2;
				codePoint = lead & 0x0F;
				minimum = 0x800;
			}
			else if (lead >= 0xF0 && lead <= 0xF4)
			{
				continuationCount = 3;
				codePoint = lead & 0x07;
				minimum = 0x10000;
			}
			else
			{
				return false;
			}
			if (continuationCount > value.length - index)
			{
				return false;
			}
			for (uint32_t offset = 0; offset < continuationCount; ++offset)
			{
				const uint32_t byte = bytes[index++];
				if ((byte & 0xC0) != 0x80)
				{
					return false;
				}
				codePoint = (codePoint << 6) | (byte & 0x3F);
			}
			if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
			{
				return false;
			}
		}
		return true;
	}

	AetherGameplayStatus CopyGameplayUtf8(AetherGameplayUtf8 source, AetherGameplayUtf8Buffer destination) noexcept
	{
		if (destination.required == nullptr || source.length == std::numeric_limits<uint32_t>::max() || !IsGameplayUtf8(source) || (destination.data == nullptr && destination.capacity != 0))
		{
			if (destination.data != nullptr && destination.capacity != 0)
			{
				destination.data[0] = '\0';
			}
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		*destination.required = source.length + 1;
		if (destination.capacity < *destination.required)
		{
			if (destination.data != nullptr && destination.capacity != 0)
			{
				destination.data[0] = '\0';
			}
			return AE_GAMEPLAY_BUFFER_TOO_SMALL;
		}
		if (source.length != 0)
		{
			std::memmove(destination.data, source.data, source.length);
		}
		destination.data[source.length] = '\0';
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus DispatchGameplayEvent(const AetherGameplayModuleApi& api, AetherGameplayInstance instance, const AetherGameplayEvent& event) noexcept
	{
		const auto validation = ValidateGameplayModule(api);
		if (validation != AE_GAMEPLAY_OK)
		{
			return validation;
		}
		if (event.structSize != sizeof(event) || (event.payload.length != 0 && event.payload.data == nullptr))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		uint64_t capability = 0;
		switch (event.kind)
		{
			case AE_GAMEPLAY_EVENT_PHYSICS:
				capability = AE_GAMEPLAY_CAP_PHYSICS;
				break;
			case AE_GAMEPLAY_EVENT_OWNERSHIP:
				capability = AE_GAMEPLAY_CAP_OWNERSHIP;
				break;
			case AE_GAMEPLAY_EVENT_NETWORK:
				capability = AE_GAMEPLAY_CAP_NETWORK;
				break;
			default:
				return AE_GAMEPLAY_UNSUPPORTED;
		}
		if ((api.capabilities & capability) == 0 || api.dispatchEvent == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		try
#endif
		{
			return api.dispatchEvent(api.context, instance, &event);
		}
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		catch (...)
		{
			return AE_GAMEPLAY_SCRIPT_ERROR;
		}
#endif
	}
} // namespace aether::scripting
