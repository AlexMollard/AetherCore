#include "GameplayAbiFixture.hpp"

#include <cstdio>
#include <cstring>
#include <iterator>

namespace
{
	uint32_t s_checks = 0;
	uint32_t s_failures = 0;
	uint32_t s_callbacks = 0;
	AetherGameplayVec3 s_position{};
	void Check(bool condition, const char* name)
	{
		++s_checks;
		if (!condition)
		{
			++s_failures;
			std::fprintf(stderr, "FAIL: %s\n", name);
		}
	}
	bool Empty(const AetherGameplayModuleApi& api)
	{
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&api);
		for (size_t i = 0; i < sizeof(api); ++i)
		{
			if (bytes[i] != 0) { return false; }
		}
		return true;
	}
	AetherGameplayStatus Valid(uint64_t context, AetherGameplayEntity entity, uint32_t* result)
	{
		++s_callbacks;
		*result = context == 77 && entity.world == 11 && entity.id == 42;
		return AE_GAMEPLAY_OK;
	}
	AetherGameplayStatus Get(uint64_t context, AetherGameplayEntity, AetherGameplayVec3* result)
	{
		++s_callbacks;
		*result = s_position;
		return context == 77 ? AE_GAMEPLAY_OK : AE_GAMEPLAY_INVALID_ARGUMENT;
	}
	AetherGameplayStatus Set(uint64_t context, AetherGameplayEntity, AetherGameplayVec3 value)
	{
		++s_callbacks;
		s_position = value;
		return context == 77 ? AE_GAMEPLAY_OK : AE_GAMEPLAY_INVALID_ARGUMENT;
	}
	AetherGameplayStatus Diagnostic(uint64_t context, const AetherGameplayDiagnostic* diagnostic)
	{
		++s_callbacks;
		Check(context == 77 && diagnostic->structSize == sizeof(*diagnostic), "diagnostic record");
		Check(diagnostic->message.length == 9 && std::memcmp(diagnostic->message.data, "caf\xc3\xa9 \xe7\x8c\xab", 9) == 0, "diagnostic UTF8 bytes");
		return AE_GAMEPLAY_OK;
	}
	AetherGameplayStatus Missing(uint32_t, uint32_t, AetherGameplayModuleApi* outApi)
	{
		aether_gameplay_query(AE_GAMEPLAY_ABI_MAJOR, sizeof(*outApi), outApi);
		outApi->update = nullptr;
		return AE_GAMEPLAY_OK;
	}
	AetherGameplayStatus ThrowQuery(uint32_t, uint32_t, AetherGameplayModuleApi*)
	{
		throw 7;
	}
	AetherGameplayStatus ThrowEvent(uint64_t, AetherGameplayInstance, const AetherGameplayEvent*)
	{
		throw 7;
	}
}

int main()
{
	using namespace aether::scripting;
	AetherGameplayModuleApi api{};
	Check(QueryGameplayModule(aether_gameplay_query, AE_GAMEPLAY_ABI_MAJOR, sizeof(api), &api) == AE_GAMEPLAY_OK, "query accepted");
	if (!api.initialize)
	{
		std::printf("{\"schemaVersion\":1,\"probe\":\"gameplay_abi\",\"status\":\"failed\",\"checks\":%u,\"failures\":%u}\n", s_checks, s_failures);
		return 1;
	}
	const auto original = api;
	Check(aether_gameplay_query(999, sizeof(api), &api) == AE_GAMEPLAY_ABI_MISMATCH && Empty(api), "Rust query major rejection clears stale table");
	api = original;
	Check(aether_gameplay_query(AE_GAMEPLAY_ABI_MAJOR, sizeof(api) - 1, &api) == AE_GAMEPLAY_BUFFER_TOO_SMALL && api.initialize == nullptr, "Rust query short table rejection");
	Check(aether_gameplay_query(AE_GAMEPLAY_ABI_MAJOR, sizeof(api), nullptr) == AE_GAMEPLAY_INVALID_ARGUMENT, "Rust query null output rejection");
	api = original;
	Check(QueryGameplayModule(ThrowQuery, AE_GAMEPLAY_ABI_MAJOR, sizeof(api), &api) == AE_GAMEPLAY_SCRIPT_ERROR && Empty(api), "C++ query exception contained");
	const size_t mandatoryCallbacks[] = {
		offsetof(AetherGameplayModuleApi, initialize), offsetof(AetherGameplayModuleApi, shutdown),
		offsetof(AetherGameplayModuleApi, getTypeCount), offsetof(AetherGameplayModuleApi, getTypeInfo),
		offsetof(AetherGameplayModuleApi, getPropertyInfo), offsetof(AetherGameplayModuleApi, create),
		offsetof(AetherGameplayModuleApi, destroy), offsetof(AetherGameplayModuleApi, attach),
		offsetof(AetherGameplayModuleApi, update), offsetof(AetherGameplayModuleApi, detach),
		offsetof(AetherGameplayModuleApi, getProperty), offsetof(AetherGameplayModuleApi, setProperty),
		offsetof(AetherGameplayModuleApi, getDefaultProperty), offsetof(AetherGameplayModuleApi, captureStreamingState),
		offsetof(AetherGameplayModuleApi, restoreStreamingState)};
	for (const auto offset : mandatoryCallbacks)
	{
		auto incomplete = original;
		std::memset(reinterpret_cast<unsigned char*>(&incomplete) + offset, 0, sizeof(api.initialize));
		api = original;
		Check(PublishGameplayModule(incomplete, AE_GAMEPLAY_ABI_MAJOR, sizeof(api), &api) == AE_GAMEPLAY_INVALID_ARGUMENT && Empty(api), "each mandatory callback rejected with empty output");
	}
	api = original;
	Check(QueryGameplayModule(aether_gameplay_query, 999, sizeof(api), &api) == AE_GAMEPLAY_ABI_MISMATCH && Empty(api), "major rejection clears stale table");
	api = original;
	Check(QueryGameplayModule(Missing, AE_GAMEPLAY_ABI_MAJOR, sizeof(api), &api) == AE_GAMEPLAY_INVALID_ARGUMENT && Empty(api), "missing callback clears stale table");
	api = original;
	Check(QueryGameplayModule(aether_gameplay_query, AE_GAMEPLAY_ABI_MAJOR, sizeof(api) - 1, &api) == AE_GAMEPLAY_BUFFER_TOO_SMALL && api.initialize == nullptr, "short table rejected");
	api = original;
	api.structSize -= 1;
	Check(ValidateGameplayModule(api) == AE_GAMEPLAY_ABI_MISMATCH, "module struct size rejected");
	api = original;
	AetherGameplayEngineApi engine{sizeof(engine), AE_GAMEPLAY_ABI_MAJOR, 0, 77, Valid, Get, Set, Diagnostic};
	auto badEngine = engine;
	badEngine.abiMajor = 999;
	Check(api.initialize(api.context, &badEngine) == AE_GAMEPLAY_ABI_MISMATCH, "Rust rejects engine ABI major");
	badEngine = engine;
	badEngine.structSize -= 1;
	Check(api.initialize(api.context, &badEngine) == AE_GAMEPLAY_ABI_MISMATCH, "Rust rejects engine table size");
	badEngine = engine;
	badEngine.setPosition = nullptr;
	Check(api.initialize(api.context, &badEngine) == AE_GAMEPLAY_INVALID_ARGUMENT, "Rust rejects missing engine callback");
	Check(api.initialize(api.context, &engine) == AE_GAMEPLAY_OK, "initialize");
	uint32_t count = 0;
	Check(api.getTypeCount(api.context, &count) == AE_GAMEPLAY_OK && count == 1, "type enumeration");
	AetherGameplayTypeInfo type{sizeof(type)};
	Check(api.getTypeInfo(api.context, 0, &type) == AE_GAMEPLAY_OK && type.propertyCount == 1 && type.name.length == 9 && std::memcmp(type.name.data, "caf\xc3\xa9 \xe7\x8c\xab", 9) == 0, "nonASCII type descriptor");
	AetherGameplayPropertyInfo property{sizeof(property)};
	Check(api.getPropertyInfo(api.context, 0, 0, &property) == AE_GAMEPLAY_OK && property.type == AE_GAMEPLAY_PROPERTY_STRING && property.name.length == 3 && std::memcmp(property.name.data, "\xe5\x90\x8d", 3) == 0, "nonASCII property descriptor");
	AetherGameplayInstance instance = 0;
	Check(api.create(api.context, 0, {11, 42, 0}, &instance) == AE_GAMEPLAY_OK && instance != 0, "create");
	Check(api.update(api.context, instance, 0.5f) == AE_GAMEPLAY_INVALID_HANDLE, "update before attach rejected");
	Check(api.attach(api.context, instance) == AE_GAMEPLAY_OK, "attach");
	Check(api.update(api.context, instance, 0.5f) == AE_GAMEPLAY_OK && s_position.x == 0.5f && s_callbacks == 4, "update invokes all engine callbacks");
	Check(api.update(api.context, instance, -1.0f) == AE_GAMEPLAY_SCRIPT_ERROR, "script error propagated");
	AetherGameplayPropertyValue value{sizeof(value)};
	uint32_t required = 0;
	char small[4] = {'X', 'Y', 'Z', '!'};
	Check(api.getProperty(api.context, instance, 0, &value, {small, 3, &required}) == AE_GAMEPLAY_BUFFER_TOO_SMALL && required == 10 && small[1] == 'Y' && small[2] == 'Z' && small[3] == '!', "property short buffer preserves canary");
	char buffer[32]{};
	Check(api.getDefaultProperty(api.context, 0, 0, &value, {buffer, sizeof(buffer), &required}) == AE_GAMEPLAY_OK && value.type == AE_GAMEPLAY_PROPERTY_STRING && required == 10 && std::memcmp(buffer, "caf\xc3\xa9 \xe7\x8c\xab", 10) == 0, "default caller buffer UTF8");
	Check(api.getDefaultProperty(api.context, 0, 0, &value, {small, 3, &required}) == AE_GAMEPLAY_BUFFER_TOO_SMALL && small[3] == '!', "default short buffer canary");
	Check(api.setProperty(api.context, instance, 0, &value, {"new\0name", 8}) == AE_GAMEPLAY_OK, "set explicit length with embedded NUL");
	Check(api.getProperty(api.context, instance, 0, &value, {buffer, sizeof(buffer), &required}) == AE_GAMEPLAY_OK && required == 9 && std::memcmp(buffer, "new\0name", 9) == 0, "get explicit length with embedded NUL");
	for (auto kind : {AE_GAMEPLAY_EVENT_PHYSICS, AE_GAMEPLAY_EVENT_OWNERSHIP, AE_GAMEPLAY_EVENT_NETWORK})
	{
		AetherGameplayEvent event{sizeof(event), kind};
		Check(DispatchGameplayEvent(api, instance, event) == AE_GAMEPLAY_UNSUPPORTED, "optional event unsupported");
	}
	auto throwingModule = api;
	throwingModule.capabilities = AE_GAMEPLAY_CAP_PHYSICS;
	throwingModule.dispatchEvent = ThrowEvent;
	const AetherGameplayEvent event{sizeof(event), AE_GAMEPLAY_EVENT_PHYSICS};
	Check(DispatchGameplayEvent(throwingModule, instance, event) == AE_GAMEPLAY_SCRIPT_ERROR, "C++ event exception contained");
	Check(api.destroy(api.context, instance) == AE_GAMEPLAY_INVALID_HANDLE, "destroy before detach rejected");
	Check(api.detach(api.context, instance) == AE_GAMEPLAY_OK, "detach");
	Check(api.update(api.context, instance, 0.1f) == AE_GAMEPLAY_INVALID_HANDLE, "update after detach rejected");
	Check(api.destroy(api.context, instance) == AE_GAMEPLAY_OK, "destroy");
	Check(api.attach(api.context, instance) == AE_GAMEPLAY_INVALID_HANDLE, "stale instance rejected");
	Check(api.create(api.context, 0, {11, 99, 0}, &instance) == AE_GAMEPLAY_OK, "create failed attach instance");
	Check(api.attach(api.context, instance) == AE_GAMEPLAY_SCRIPT_ERROR, "failed attach status");
	Check(api.destroy(api.context, instance) == AE_GAMEPLAY_OK, "failed attach still destroys");
	Check(api.shutdown(api.context) == AE_GAMEPLAY_OK, "shutdown");
	// Layout values are appended below from the canonical field list.
	const size_t layout[] = {sizeof(AetherGameplayUtf8),
		alignof(AetherGameplayUtf8),
		offsetof(AetherGameplayUtf8, data),
		offsetof(AetherGameplayUtf8, length),
		sizeof(AetherGameplayUtf8Buffer),
		alignof(AetherGameplayUtf8Buffer),
		offsetof(AetherGameplayUtf8Buffer, data),
		offsetof(AetherGameplayUtf8Buffer, capacity),
		offsetof(AetherGameplayUtf8Buffer, required),
		sizeof(AetherGameplayEntity),
		alignof(AetherGameplayEntity),
		offsetof(AetherGameplayEntity, world),
		offsetof(AetherGameplayEntity, id),
		offsetof(AetherGameplayEntity, reserved),
		sizeof(AetherGameplayVec3),
		alignof(AetherGameplayVec3),
		offsetof(AetherGameplayVec3, x),
		offsetof(AetherGameplayVec3, y),
		offsetof(AetherGameplayVec3, z),
		sizeof(AetherGameplayPropertyValue),
		alignof(AetherGameplayPropertyValue),
		offsetof(AetherGameplayPropertyValue, structSize),
		offsetof(AetherGameplayPropertyValue, type),
		offsetof(AetherGameplayPropertyValue, vector),
		offsetof(AetherGameplayPropertyValue, integer),
		offsetof(AetherGameplayPropertyValue, entity),
		sizeof(AetherGameplayTypeInfo),
		alignof(AetherGameplayTypeInfo),
		offsetof(AetherGameplayTypeInfo, structSize),
		offsetof(AetherGameplayTypeInfo, propertyCount),
		offsetof(AetherGameplayTypeInfo, name),
		sizeof(AetherGameplayPropertyInfo),
		alignof(AetherGameplayPropertyInfo),
		offsetof(AetherGameplayPropertyInfo, structSize),
		offsetof(AetherGameplayPropertyInfo, type),
		offsetof(AetherGameplayPropertyInfo, name),
		offsetof(AetherGameplayPropertyInfo, enumNames),
		offsetof(AetherGameplayPropertyInfo, componentType),
		sizeof(AetherGameplayEvent),
		alignof(AetherGameplayEvent),
		offsetof(AetherGameplayEvent, structSize),
		offsetof(AetherGameplayEvent, kind),
		offsetof(AetherGameplayEvent, other),
		offsetof(AetherGameplayEvent, code),
		offsetof(AetherGameplayEvent, flags),
		offsetof(AetherGameplayEvent, payload),
		sizeof(AetherGameplayDiagnostic),
		alignof(AetherGameplayDiagnostic),
		offsetof(AetherGameplayDiagnostic, structSize),
		offsetof(AetherGameplayDiagnostic, severity),
		offsetof(AetherGameplayDiagnostic, code),
		offsetof(AetherGameplayDiagnostic, message),
		offsetof(AetherGameplayDiagnostic, file),
		offsetof(AetherGameplayDiagnostic, line),
		offsetof(AetherGameplayDiagnostic, column),
		sizeof(AetherGameplayEngineApi),
		alignof(AetherGameplayEngineApi),
		offsetof(AetherGameplayEngineApi, structSize),
		offsetof(AetherGameplayEngineApi, abiMajor),
		offsetof(AetherGameplayEngineApi, capabilities),
		offsetof(AetherGameplayEngineApi, context),
		offsetof(AetherGameplayEngineApi, entityValid),
		offsetof(AetherGameplayEngineApi, getPosition),
		offsetof(AetherGameplayEngineApi, setPosition),
		offsetof(AetherGameplayEngineApi, reportDiagnostic),
		sizeof(AetherGameplayModuleApi),
		alignof(AetherGameplayModuleApi),
		offsetof(AetherGameplayModuleApi, structSize),
		offsetof(AetherGameplayModuleApi, abiMajor),
		offsetof(AetherGameplayModuleApi, capabilities),
		offsetof(AetherGameplayModuleApi, context),
		offsetof(AetherGameplayModuleApi, initialize),
		offsetof(AetherGameplayModuleApi, shutdown),
		offsetof(AetherGameplayModuleApi, getTypeCount),
		offsetof(AetherGameplayModuleApi, getTypeInfo),
		offsetof(AetherGameplayModuleApi, getPropertyInfo),
		offsetof(AetherGameplayModuleApi, create),
		offsetof(AetherGameplayModuleApi, destroy),
		offsetof(AetherGameplayModuleApi, attach),
		offsetof(AetherGameplayModuleApi, update),
		offsetof(AetherGameplayModuleApi, detach),
		offsetof(AetherGameplayModuleApi, getProperty),
		offsetof(AetherGameplayModuleApi, setProperty),
		offsetof(AetherGameplayModuleApi, getDefaultProperty),
		offsetof(AetherGameplayModuleApi, dispatchEvent),
		offsetof(AetherGameplayModuleApi, captureStreamingState),
		offsetof(AetherGameplayModuleApi, restoreStreamingState)};
	for (uint32_t i = 0; i < std::size(layout); ++i)
	{
		Check(fixture_layout(i) == layout[i], "Rust/C++ record size alignment and field offset");
	}
	std::printf("{\"schemaVersion\":1,\"probe\":\"gameplay_abi\",\"status\":\"%s\",\"checks\":%u,\"failures\":%u,\"pointerBits\":%zu,\"layoutChecks\":%zu}\n", s_failures == 0 ? "passed" : "failed", s_checks, s_failures, sizeof(void*) * 8, std::size(layout));
	return s_failures == 0 ? 0 : 1;
}
