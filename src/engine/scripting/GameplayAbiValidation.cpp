#include "GameplayAbiValidation.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

#include "utils/Utf8.hpp"

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
		        || api.update == nullptr || api.detach == nullptr || api.getProperty == nullptr || api.setProperty == nullptr || api.getDefaultProperty == nullptr || api.captureStreamingState == nullptr || api.restoreStreamingState == nullptr)
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
		return IsValidUtf8(value.data, value.length);
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
