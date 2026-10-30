#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <type_traits>

#include "scripting/GameplayAbiValidation.hpp"

using namespace aether::scripting;

TEST_CASE("Gameplay ABI rejects version and size mismatch without stale callable entries")
{
	AetherGameplayModuleApi api{};
	api.context = 88;
	CHECK(QueryGameplayModule(nullptr, AE_GAMEPLAY_ABI_MAJOR + 1, sizeof(api), &api) == AE_GAMEPLAY_ABI_MISMATCH);
	CHECK(api.context == 0);
	std::array<unsigned char, sizeof(api) + 16> storage;
	storage.fill(0xA5);
	auto* out = reinterpret_cast<AetherGameplayModuleApi*>(storage.data());
	CHECK(PublishGameplayModule(api, AE_GAMEPLAY_ABI_MAJOR, 8, out) == AE_GAMEPLAY_BUFFER_TOO_SMALL);
	for (size_t i = 0; i < 8; ++i)
	{
		CHECK(storage[i] == 0);
	}
	CHECK(storage[8] == 0xA5);
	CHECK(PublishGameplayModule(api, AE_GAMEPLAY_ABI_MAJOR, sizeof(api) + 16, out) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(storage[sizeof(api)] == 0xA5);
	CHECK(ValidateGameplayModule(api) == AE_GAMEPLAY_ABI_MISMATCH);
	api.structSize = sizeof(api);
	api.abiMajor = AE_GAMEPLAY_ABI_MAJOR;
	CHECK(ValidateGameplayModule(api) == AE_GAMEPLAY_INVALID_ARGUMENT);
}

TEST_CASE("Gameplay UTF8 results report bytes and do not split non ASCII strings")
{
	const char value[] = "\xE7\x8E\xA9\xE5\xAE\xB6";
	char buffer[8] = {'x'};
	uint32_t required = 0;
	CHECK(CopyGameplayUtf8({value, 6}, {buffer, 6, &required}) == AE_GAMEPLAY_BUFFER_TOO_SMALL);
	CHECK(required == 7);
	CHECK(buffer[0] == 0);
	CHECK(CopyGameplayUtf8({value, 6}, {buffer, 7, &required}) == AE_GAMEPLAY_OK);
	CHECK(std::memcmp(value, buffer, 7) == 0);
	CHECK(CopyGameplayUtf8({value, 6}, {nullptr, 0, &required}) == AE_GAMEPLAY_BUFFER_TOO_SMALL);
	CHECK(CopyGameplayUtf8({nullptr, 1}, {buffer, 8, &required}) == AE_GAMEPLAY_INVALID_ARGUMENT);
}

TEST_CASE("Gameplay property values have no process-local pointer storage")
{
	CHECK(sizeof(AetherGameplayEntity) == 16);
	CHECK(sizeof(AetherGameplayPropertyValue) == 48);
	CHECK(std::is_standard_layout_v<AetherGameplayPropertyValue>);
	CHECK(std::is_trivially_copyable_v<AetherGameplayPropertyValue>);
}

namespace
{
	AetherGameplayModuleApi CompleteModule()
	{
		AetherGameplayModuleApi api{};
		api.structSize = sizeof(api);
		api.abiMajor = AE_GAMEPLAY_ABI_MAJOR;
		api.context = 123;
		api.initialize = [](uint64_t, const AetherGameplayEngineApi*) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.shutdown = [](uint64_t) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.getTypeCount = [](uint64_t, uint32_t* count) -> int32_t
		{
			*count = 1;
			return AE_GAMEPLAY_OK;
		};
		api.getTypeInfo = [](uint64_t, uint32_t, AetherGameplayTypeInfo*) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.getPropertyInfo = [](uint64_t, uint32_t, uint32_t, AetherGameplayPropertyInfo*) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.create = [](uint64_t, uint32_t, AetherGameplayEntity, uint64_t*) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.destroy = [](uint64_t, uint64_t) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.attach = api.destroy;
		api.detach = api.destroy;
		api.update = [](uint64_t, uint64_t, float) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.getProperty = [](uint64_t, uint64_t, uint32_t, AetherGameplayPropertyValue*, AetherGameplayUtf8Buffer) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.setProperty = [](uint64_t, uint64_t, uint32_t, const AetherGameplayPropertyValue*, AetherGameplayUtf8) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.getDefaultProperty = [](uint64_t, uint32_t, uint32_t, AetherGameplayPropertyValue*, AetherGameplayUtf8Buffer) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		api.captureStreamingState = [](uint64_t, AetherGameplayInstance, AetherGameplayUtf8Buffer output) -> int32_t
		{
			return CopyGameplayUtf8({nullptr, 0}, output);
		};
		api.restoreStreamingState = [](uint64_t, AetherGameplayInstance, AetherGameplayUtf8) -> int32_t
		{
			return AE_GAMEPLAY_OK;
		};
		return api;
	}
} // namespace

TEST_CASE("Gameplay query accepts complete tables and clears failures before exposing callbacks")
{
	AetherGameplayModuleApi api{};
	auto query = +[](uint32_t major, uint32_t size, AetherGameplayModuleApi* out) -> int32_t
	{
		return PublishGameplayModule(CompleteModule(), major, size, out);
	};
	REQUIRE(QueryGameplayModule(query, AE_GAMEPLAY_ABI_MAJOR, sizeof(api), &api) == AE_GAMEPLAY_OK);
	CHECK(api.context == 123);
	REQUIRE(api.update != nullptr);
	auto missing = CompleteModule();
	missing.getDefaultProperty = nullptr;
	CHECK(PublishGameplayModule(missing, AE_GAMEPLAY_ABI_MAJOR, sizeof(api), &api) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(api.update == nullptr);
	auto failure = +[](uint32_t, uint32_t, AetherGameplayModuleApi* out) -> int32_t
	{
		*out = CompleteModule();
		return AE_GAMEPLAY_SCRIPT_ERROR;
	};
	CHECK(QueryGameplayModule(failure, AE_GAMEPLAY_ABI_MAJOR, sizeof(api), &api) == AE_GAMEPLAY_SCRIPT_ERROR);
	CHECK(api.context == 0);
	CHECK(api.update == nullptr);
}

TEST_CASE("Gameplay optional events refuse unsupported operations and propagate callback errors")
{
	auto api = CompleteModule();
	AetherGameplayEvent event{};
	event.structSize = sizeof(event);
	event.kind = AE_GAMEPLAY_EVENT_PHYSICS;
	CHECK(DispatchGameplayEvent(api, 1, event) == AE_GAMEPLAY_UNSUPPORTED);
	api.capabilities = AE_GAMEPLAY_CAP_PHYSICS;
	CHECK(ValidateGameplayModule(api) == AE_GAMEPLAY_INVALID_ARGUMENT);
	api.dispatchEvent = [](uint64_t, uint64_t, const AetherGameplayEvent*) -> int32_t
	{
		return AE_GAMEPLAY_SCRIPT_ERROR;
	};
	CHECK(ValidateGameplayModule(api) == AE_GAMEPLAY_OK);
	CHECK(DispatchGameplayEvent(api, 1, event) == AE_GAMEPLAY_SCRIPT_ERROR);
	event.kind = AE_GAMEPLAY_EVENT_NETWORK;
	CHECK(DispatchGameplayEvent(api, 1, event) == AE_GAMEPLAY_UNSUPPORTED);
	event.kind = 999;
	CHECK(DispatchGameplayEvent(api, 1, event) == AE_GAMEPLAY_UNSUPPORTED);
}

TEST_CASE("Gameplay UTF8 rejects malformed and overlong code points")
{
	CHECK(IsGameplayUtf8({"\xF0\x9F\x8E\xAE", 4}));
	CHECK_FALSE(IsGameplayUtf8({"\xC0\xAF", 2}));
	CHECK_FALSE(IsGameplayUtf8({"\xED\xA0\x80", 3}));
	CHECK_FALSE(IsGameplayUtf8({"\xF4\x90\x80\x80", 4}));
	CHECK_FALSE(IsGameplayUtf8({"\xE7\x8E", 2}));
	CHECK_FALSE(IsGameplayUtf8({"\x80", 1}));
	CHECK(IsGameplayUtf8({nullptr, 0}));
	char buffer[] = "alias";
	uint32_t required = 0;
	CHECK(CopyGameplayUtf8({buffer, 5}, {buffer, sizeof(buffer), &required}) == AE_GAMEPLAY_OK);
	CHECK(std::strcmp(buffer, "alias") == 0);
}

TEST_CASE("Gameplay ABI rejects unknown capability flags")
{
	auto api = CompleteModule();
	api.capabilities = UINT64_C(1) << 63;
	CHECK(ValidateGameplayModule(api) == AE_GAMEPLAY_INVALID_ARGUMENT);
}
