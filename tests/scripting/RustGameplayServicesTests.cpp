#include <doctest/doctest.h>

#include <limits>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "scene/World.hpp"
#include "io/PlatformPaths.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/RustGameplayServices.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/GameplaySaveStore.hpp"
#include "ui/UiComponents.hpp"

// These assertions mirror rust/aethercore-abi/src/services.rs without changing ABI v1.
static_assert(sizeof(void*) == 4 || sizeof(void*) == 8);
static_assert(offsetof(AetherGameplayServiceRequest, structSize) == 0);
static_assert(offsetof(AetherGameplayServiceRequest, operation) == 4);
static_assert(offsetof(AetherGameplayServiceRequest, entity) == 8);
static_assert(offsetof(AetherGameplayServiceRequest, vector) == 24);
static_assert(offsetof(AetherGameplayServiceRequest, integer) == 40);
static_assert(offsetof(AetherGameplayServiceRequest, text) == 48);
static_assert(offsetof(AetherGameplayServiceRequest, text2) == (sizeof(void*) == 8 ? 64 : 56));
static_assert(sizeof(AetherGameplayServiceRequest) == (sizeof(void*) == 8 ? 80 : 64));
static_assert(offsetof(AetherGameplayServicesApi, structSize) == 0);
static_assert(offsetof(AetherGameplayServicesApi, abiMajor) == 4);
static_assert(offsetof(AetherGameplayServicesApi, context) == 8);
static_assert(offsetof(AetherGameplayServicesApi, capabilities) == 16);
static_assert(offsetof(AetherGameplayServicesApi, invoke) == 24);
static_assert(sizeof(decltype(AetherGameplayServicesApi::invoke)) == sizeof(void*));
static_assert(sizeof(AetherGameplayServicesApi) == (sizeof(void*) == 8 || alignof(uint64_t) == 8 ? 32 : 28));

using namespace aether;
using namespace aether::app::scripting;

TEST_CASE("Shared gameplay saves preserve bytes, enforce keys, and honor read isolation")
{
	const auto directory = std::filesystem::temp_directory_path() / ("aether-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	GameplaySaveStore store(directory);
	const std::string payload("score=42\0caf\xc3\xa9", 14);
	CHECK(store.Write("score", payload) == AE_GAMEPLAY_OK);
	std::string output = "unchanged";
	CHECK(store.Read("score", output, false) == AE_GAMEPLAY_INVALID_HANDLE);
	CHECK(output == "unchanged");
	REQUIRE(store.Read("score", output, true) == AE_GAMEPLAY_OK);
	CHECK(output == payload);
	CHECK(store.Write("../escape", payload) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(store.Write("", payload) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(store.Read("missing", output, true) == AE_GAMEPLAY_INVALID_HANDLE);
	CHECK(GameplaySaveStore({}).Write("score", payload) == AE_GAMEPLAY_UNSUPPORTED);
	std::error_code error;
	std::filesystem::remove(directory / "score.txt", error);
	CHECK_FALSE(error);
	std::filesystem::remove(directory, error);
	CHECK_FALSE(error);
}

namespace
{
	struct ReentrantProbe
	{
		AetherGameplayServicesApi api;
		AetherGameplayStatus status = AE_GAMEPLAY_OK;

		void OnName(entt::registry&, entt::entity)
		{
			AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_KEY_DOWN};
			AetherGameplayPropertyValue value{.structSize = sizeof(value)};
			status = api.invoke(api.context, &request, &value, {});
		}
	};
} // namespace

TEST_CASE("Rust services reject invalid contexts, threads, handles, and malformed requests")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	SceneContext context{.world = &world};
	AetherGameplayServicesApi copied;
	{
		RustGameplayServices services(context, access);
		copied = services.EngineApi();
		AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_KEY_DOWN};
		AetherGameplayPropertyValue value{.structSize = sizeof(value)};
		CHECK(copied.invoke(copied.context, &request, &value, {}) == AE_GAMEPLAY_UNSUPPORTED);
		AetherGameplayStatus workerStatus = AE_GAMEPLAY_OK;
		std::thread worker([&] { workerStatus = copied.invoke(copied.context, &request, &value, {}); });
		worker.join();
		CHECK(workerStatus == AE_GAMEPLAY_WRONG_THREAD);
		request.operation = AE_GAMEPLAY_SERVICE_SET_SPRITE_COLOR;
		request.entity = access.Handle(world.Create());
		request.entity.world++;
		CHECK(copied.invoke(copied.context, &request, &value, {}) == AE_GAMEPLAY_WRONG_WORLD);
		request.entity.world--;
		request.vector[0] = std::numeric_limits<float>::infinity();
		CHECK(copied.invoke(copied.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_ARGUMENT);
		request.vector[0] = 0;
		world.Destroy(Entity{request.entity.id});
		CHECK(copied.invoke(copied.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_HANDLE);
		request = {.structSize = sizeof(request), .operation = 999};
		CHECK(copied.invoke(copied.context, &request, &value, {}) == AE_GAMEPLAY_UNSUPPORTED);
		request.structSize--;
		CHECK(copied.invoke(copied.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	}
	AetherGameplayServiceRequest request{.structSize = sizeof(request)};
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	CHECK(copied.invoke(copied.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_HANDLE);
}

TEST_CASE("Rust services reject a mismatched world and invalidated access before mutation")
{
	World world;
	World other;
	aether::scripting::GameplayAccess access(world);
	SceneContext context{.world = &other};
	RustGameplayServices services(context, access);
	const auto api = services.EngineApi();
	AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_CREATE_ENTITY};
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_WRONG_WORLD);
	context.world = &world;
	access.Invalidate();
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_HANDLE);
}

TEST_CASE("Rust services reject nested native service invocations")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	SceneContext context{.world = &world};
	RustGameplayServices services(context, access);
	const auto api = services.EngineApi();
	ReentrantProbe probe{api};
	world.GetRegistry().on_construct<NameComponent>().connect<&ReentrantProbe::OnName>(probe);
	AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_CREATE_ENTITY};
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_OK);
	CHECK(probe.status == AE_GAMEPLAY_REENTRANT);
	world.GetRegistry().on_construct<NameComponent>().disconnect<&ReentrantProbe::OnName>(probe);
}

TEST_CASE("Rust services copy strings and defer destructive operations")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	SceneContext context{.world = &world};
	RustGameplayServices services(context, access);
	const auto api = services.EngineApi();
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	char name[] = "spawn";
	AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_CREATE_ENTITY, .text = {name, 5}};
	REQUIRE(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_OK);
	REQUIRE(value.type == AE_GAMEPLAY_PROPERTY_ENTITY);
	const auto handle = value.entity;
	name[0] = 'X';
	CHECK(world.TryGet<NameComponent>(Entity{handle.id})->name == "spawn");
	world.Emplace<ui::UIText>(Entity{handle.id});
	request = {.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_SET_UI_TEXT, .entity = handle, .text = {name, 5}};
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_OK);
	name[1] = 'X';
	CHECK(world.TryGet<ui::UIText>(Entity{handle.id})->text == "Xpawn");
	const char invalid[] = {char(0xc0), char(0x80)};
	request.text = {invalid, 2};
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	request = {.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_DESTROY_ENTITY, .entity = handle};
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_OK);
	CHECK(context.pendingDestroys.size() == 1);
	CHECK(world.GetRegistry().valid(World::ToEntt(Entity{handle.id})));
}

TEST_CASE("Rust save services persist exact UTF8 lengths and never truncate output")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	SceneContext context{.world = &world};
	RustGameplayServices services(context, access);
	const auto api = services.EngineApi();
	const auto key = "sdk-unit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const char payload[] = {'o', '\0', 'k'};
	AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_WRITE_SAVE, .text = {key.data(), static_cast<uint32_t>(key.size())}, .text2 = {payload, sizeof(payload)}};
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	REQUIRE(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_OK);
	request.operation = AE_GAMEPLAY_SERVICE_READ_SAVE;
	context.allowSaveReads = false;
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_HANDLE);
	context.allowSaveReads = true;
	uint32_t required = 0;
	char shortBuffer[] = {'X', 'X'};
	CHECK(api.invoke(api.context, &request, &value, {shortBuffer, sizeof(shortBuffer), &required}) == AE_GAMEPLAY_BUFFER_TOO_SMALL);
	CHECK(required == 4);
	CHECK(shortBuffer[0] == '\0');
	CHECK(shortBuffer[1] == 'X');
	char output[4]{};
	CHECK(api.invoke(api.context, &request, &value, {output, sizeof(output), &required}) == AE_GAMEPLAY_OK);
	CHECK(value.type == AE_GAMEPLAY_PROPERTY_STRING);
	CHECK(std::string(output, 3) == std::string(payload, 3));
	request.operation = AE_GAMEPLAY_SERVICE_WRITE_SAVE;
	request.text = {"../escape", 9};
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_ARGUMENT);

	// Remove only this test's uniquely named record, leaving existing player data.
	const char* project = std::getenv("AETHER_PROJECT_DIR");
	auto projectPath = project != nullptr && project[0] != '\0' ? std::filesystem::path(project).lexically_normal() : io::PlatformPaths::GetExecutableDir();
	if (projectPath.filename().empty())
	{
		projectPath = projectPath.parent_path();
	}
	std::string nameSpace = "project-";
	for (unsigned char byte: projectPath.filename().u8string())
	{
		constexpr char kHex[] = "0123456789abcdef";
		nameSpace += kHex[byte >> 4];
		nameSpace += kHex[byte & 15];
	}
	std::error_code error;
	std::filesystem::remove(io::PlatformPaths::GetUserConfigDir() / "rust-saves" / nameSpace / (key + ".txt"), error);
	CHECK_FALSE(error);
}
