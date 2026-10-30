#include <doctest/doctest.h>

#include <limits>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <array>
#include <filesystem>
#include <memory>
#include <string_view>
#include <tuple>

#include "assets/GltfAsset.hpp"
#include "io/FileSystem.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "platform/Input.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scripting/Gameplay3D.hpp"
#include "rendering/CustomPassRegistry.hpp"
#include "utils/ServiceContainer.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayAbiValidation.hpp"
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
TEST_CASE("Rust pointer and Euler services negotiate and validate inputs")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	SceneContext withoutInput{.world = &world};
	RustGameplayServices unavailable(withoutInput, access);
	const auto noInput = unavailable.EngineApi();
	for (const auto op: {AE_GAMEPLAY_SERVICE_MOUSE_DOWN, AE_GAMEPLAY_SERVICE_MOUSE_DELTA, AE_GAMEPLAY_SERVICE_SCROLL_DELTA})
	{
		CHECK((noInput.capabilities & AE_GAMEPLAY_SERVICE_CAP(op)) == 0);
	}
	CHECK((noInput.capabilities & AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_SET_EULER)) != 0);
	Input input;
	SceneContext context{.world = &world, .input = &input};
	RustGameplayServices services(context, access);
	const auto api = services.EngineApi();
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_MOUSE_DOWN, .integer = 3};
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK((api.capabilities & AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_MOUSE_DOWN)) != 0);
	request.integer = 0;
	input.SetSyntheticMouseButton(0, true);
	input.Update();
	REQUIRE(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_OK);
	CHECK(value.type == AE_GAMEPLAY_PROPERTY_BOOL);
	CHECK(value.integer == 1);
	const auto entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	request = {.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_SET_EULER, .entity = access.Handle(entity), .vector = {0, 45, 0, 0}};
	REQUIRE(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_OK);
	glm::vec3 position{}, euler{}, scale{};
	DecomposeTRS(world.Get<TransformComponent>(entity).localToWorld, position, euler, scale);
	CHECK(euler.y == doctest::Approx(45));
	request.vector[0] = std::numeric_limits<float>::infinity();
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	request.vector[0] = 0;
	request.entity.world++;
	CHECK(api.invoke(api.context, &request, &value, {}) == AE_GAMEPLAY_WRONG_WORLD);
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

namespace
{
	AetherGameplayUtf8 Utf8(const std::string& text)
	{
		return {text.data(), static_cast<uint32_t>(text.size())};
	}

	// Three header float4 (params, color0, color1) followed by `count` data float4 with distinct lanes.
	std::vector<float> CustomPassPayload(size_t count)
	{
		std::vector<float> payload;
		for (size_t i = 0; i < 3 + count; ++i)
		{
			for (size_t lane = 0; lane < 4; ++lane)
			{
				payload.push_back(static_cast<float>(i * 4 + lane));
			}
		}
		return payload;
	}

	AetherGameplayStatus InvokeCustomPass(const AetherGameplayServicesApi& api, uint32_t operation, const std::string& name, AetherGameplayUtf8 text2 = {}, int64_t integer = 0)
	{
		AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = operation, .integer = integer, .text = Utf8(name), .text2 = text2};
		AetherGameplayPropertyValue value{.structSize = sizeof(value)};
		return api.invoke(api.context, &request, &value, {});
	}

	AetherGameplayUtf8 Bytes(const std::vector<float>& payload)
	{
		return {reinterpret_cast<const char*>(payload.data()), static_cast<uint32_t>(payload.size() * sizeof(float))};
	}
} // namespace

TEST_CASE("Rust custom pass services register, submit with the 4096 clamp, and no-op for unknown passes")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	ServiceContainer services;
	CustomPassRegistry registry;
	services.Register(registry);
	SceneContext context{.world = &world, .services = &services};
	RustGameplayServices host(context, access);
	const auto api = host.EngineApi();
	for (const auto operation: {AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, AE_GAMEPLAY_SERVICE_UNREGISTER_CUSTOM_PASS, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS})
	{
		CHECK((api.capabilities & AE_GAMEPLAY_SERVICE_CAP(operation)) != 0);
	}

	const auto payload = CustomPassPayload(2);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", Bytes(payload)) == AE_GAMEPLAY_OK);
	CHECK(registry.frame.passes.empty());

	const std::string shader = "ink_field";
	REQUIRE(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, "ink", Utf8(shader), 1) == AE_GAMEPLAY_OK);
	REQUIRE(registry.registered.contains("ink"));
	CHECK(registry.registered["ink"].shader == "ink_field");
	CHECK(registry.registered["ink"].stage == CustomPassStage::OverScene2D);

	REQUIRE(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", Bytes(payload)) == AE_GAMEPLAY_OK);
	REQUIRE(registry.frame.passes.size() == 1);
	const auto& draw = registry.frame.passes[0];
	CHECK(draw.shader == "ink_field");
	CHECK(draw.params == glm::vec4(0, 1, 2, 3));
	CHECK(draw.color0 == glm::vec4(4, 5, 6, 7));
	CHECK(draw.color1 == glm::vec4(8, 9, 10, 11));
	REQUIRE(draw.data.size() == 2);
	CHECK(draw.data[0] == glm::vec4(12, 13, 14, 15));
	CHECK(draw.data[1] == glm::vec4(16, 17, 18, 19));

	// A header-only payload is a zero-element submission that still draws.
	registry.frame.passes.clear();
	const auto headerOnly = CustomPassPayload(0);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", Bytes(headerOnly)) == AE_GAMEPLAY_OK);
	REQUIRE(registry.frame.passes.size() == 1);
	CHECK(registry.frame.passes[0].data.empty());

	// Elements past the shared cap are dropped, not rejected.
	registry.frame.passes.clear();
	const auto oversized = CustomPassPayload(AE_GAMEPLAY_CUSTOM_PASS_MAX_VEC4 + 10);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", Bytes(oversized)) == AE_GAMEPLAY_OK);
	REQUIRE(registry.frame.passes.size() == 1);
	CHECK(registry.frame.passes[0].data.size() == AE_GAMEPLAY_CUSTOM_PASS_MAX_VEC4);
	CHECK(registry.frame.passes[0].data.back() == glm::vec4(oversized[(3 + 4095) * 4], oversized[(3 + 4095) * 4 + 1], oversized[(3 + 4095) * 4 + 2], oversized[(3 + 4095) * 4 + 3]));

	registry.frame.passes.clear();
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_UNREGISTER_CUSTOM_PASS, "ink") == AE_GAMEPLAY_OK);
	CHECK_FALSE(registry.registered.contains("ink"));
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", Bytes(payload)) == AE_GAMEPLAY_OK);
	CHECK(registry.frame.passes.empty());
}

TEST_CASE("Rust custom pass services validate stage, payload shape, and text encodings")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	ServiceContainer services;
	CustomPassRegistry registry;
	services.Register(registry);
	SceneContext context{.world = &world, .services = &services};
	RustGameplayServices host(context, access);
	const auto api = host.EngineApi();
	const std::string shader = "ink_field";

	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, "ink", Utf8(shader), 3) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, "ink", Utf8(shader), -1) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, "", Utf8(shader), 0) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, "ink", {}, 0) == AE_GAMEPLAY_INVALID_ARGUMENT);
	const char badUtf8[] = {char(0xc0), char(0x80)};
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, "ink", {badUtf8, 2}, 0) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, std::string(badUtf8, 2), Utf8(shader), 0) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(registry.registered.empty());
	REQUIRE(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, "ink", Utf8(shader), 2) == AE_GAMEPLAY_OK);
	CHECK(registry.registered["ink"].stage == CustomPassStage::EmissiveOverLight2D);

	// Fewer than three header float4 and byte lengths that are not a whole float4 are malformed.
	const auto payload = CustomPassPayload(1);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", {}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", {Bytes(payload).data, 2 * 16}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", {Bytes(payload).data, 4 * 16 - 4}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "", Bytes(payload)) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(registry.frame.passes.empty());

	// The submit payload is raw floats: bytes that are invalid UTF-8 are accepted, while the name is still validated.
	std::vector<float> invalidUtf8Payload = CustomPassPayload(1);
	const uint32_t bad = 0xc0c0c0c0u;
	std::memcpy(&invalidUtf8Payload[0], &bad, sizeof(bad));
	REQUIRE_FALSE(aether::scripting::IsGameplayUtf8(Bytes(invalidUtf8Payload)));
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", Bytes(invalidUtf8Payload)) == AE_GAMEPLAY_OK);
	CHECK(registry.frame.passes.size() == 1);
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, std::string(badUtf8, 2), Bytes(payload)) == AE_GAMEPLAY_INVALID_ARGUMENT);
}

TEST_CASE("Rust custom pass capabilities are absent without the registry service")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	ServiceContainer services;
	SceneContext context{.world = &world, .services = &services};
	RustGameplayServices host(context, access);
	auto api = host.EngineApi();
	for (const auto operation: {AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS, AE_GAMEPLAY_SERVICE_UNREGISTER_CUSTOM_PASS, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS})
	{
		CHECK((api.capabilities & AE_GAMEPLAY_SERVICE_CAP(operation)) == 0);
	}
	CHECK(InvokeCustomPass(api, AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS, "ink", Bytes(CustomPassPayload(1))) == AE_GAMEPLAY_UNSUPPORTED);
	context.services = nullptr;
	api = host.EngineApi();
	CHECK((api.capabilities & AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS)) == 0);
	CHECK((api.capabilities & AE_GAMEPLAY_SERVICE_CAP(AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS)) == 0);
}

namespace
{
	// A real Jolt PhysicsSystem behind the Rust services, stepped at the engine's fixed timestep.
	struct Physics3DHarness
	{
		World world;
		PhysicsSystem* physics = nullptr;
		aether::scripting::GameplayAccess access{world};
		SceneContext context{};
		std::unique_ptr<RustGameplayServices> host;
		AetherGameplayServicesApi api{};
		AetherGameplayPropertyValue value{.structSize = sizeof(AetherGameplayPropertyValue)};
		char bytes[64]{};
		uint32_t required = 0;

		explicit Physics3DHarness(bool withPhysics = true)
		{
			world.SetSceneKind(SceneKind::Scene3D);
			world.SetSceneFeatures(DefaultSceneFeatures(SceneKind::Scene3D));
			context.world = &world;
			if (withPhysics)
			{
				auto system = std::make_unique<PhysicsSystem>();
				physics = system.get();
				world.RegisterSystem(std::move(system));
				context.physics = physics;
			}
			host = std::make_unique<RustGameplayServices>(context, access);
			api = host->EngineApi();
		}

		Entity MakeBody(glm::vec3 position, PhysicsMotionType motion, glm::vec3 halfExtents, float gravityFactor = 1.0f)
		{
			const Entity entity = world.Create();
			world.Emplace<TransformComponent>(entity, TransformComponent{.localToWorld = ComposeTransform(position, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
			RigidBodyComponent body;
			body.motionType = motion;
			body.gravityFactor = gravityFactor;
			world.Emplace<RigidBodyComponent>(entity, body);
			ColliderComponent collider;
			collider.shape = PhysicsShapeType::Box;
			collider.halfExtents = halfExtents;
			world.Emplace<ColliderComponent>(entity, collider);
			return entity;
		}

		void Step(float seconds)
		{
			const int frames = static_cast<int>(seconds / PhysicsSystem::kFixedTimestep + 0.5f);
			for (int i = 0; i < frames; ++i)
			{
				physics->Update(world, PhysicsSystem::kFixedTimestep);
			}
		}

		AetherGameplayStatus Call(uint32_t operation, Entity entity = {}, std::array<float, 4> vector = {}, int64_t integer = 0, const std::string& text = {}, AetherGameplayUtf8 text2 = {}, uint32_t capacity = 64)
		{
			AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = operation, .integer = integer, .text = {text.data(), static_cast<uint32_t>(text.size())}, .text2 = text2};
			request.entity = entity.IsValid() ? access.Handle(entity) : AetherGameplayEntity{};
			std::copy(vector.begin(), vector.end(), request.vector);
			value = {.structSize = sizeof(value)};
			required = 0;
			return api.invoke(api.context, &request, &value, {bytes, capacity, &required});
		}

		[[nodiscard]] glm::vec3 Vector() const
		{
			return {value.vector[0], value.vector[1], value.vector[2]};
		}

		[[nodiscard]] glm::vec3 PositionOf(Entity entity)
		{
			return world.Get<PhysicsStateComponent>(entity).currPosition;
		}
	};

	std::vector<float> RayPayload(glm::vec3 origin, glm::vec3 direction, float maxDistance)
	{
		return {origin.x, origin.y, origin.z, direction.x, direction.y, direction.z, maxDistance};
	}
} // namespace

TEST_CASE("Rust 3D velocity seeds an unbaked body, then drives the Jolt body")
{
	Physics3DHarness h;
	for (uint32_t operation = AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D; operation <= AE_GAMEPLAY_SERVICE_GET_CURRENT_CLIP; ++operation)
	{
		CHECK((h.api.capabilities & AE_GAMEPLAY_SERVICE_CAP(operation)) != 0);
	}
	const Entity body = h.MakeBody({0.0f, 10.0f, 0.0f}, PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f}, 0.0f);

	// Before the body is baked a velocity write seeds creation, so set-then-get reads it back,
	// while a force or impulse has nothing to act on and says so instead of vanishing.
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D, body, {3.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_OK);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, body) == AE_GAMEPLAY_OK);
	CHECK(h.value.type == AE_GAMEPLAY_PROPERTY_VECTOR3);
	CHECK(h.Vector() == glm::vec3(3.0f, 0.0f, 0.0f));
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_ADD_FORCE_3D, body, {1.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_NOT_READY);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_ADD_IMPULSE_3D, body, {1.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_NOT_READY);

	// One second later the seeded velocity has actually carried the body (no gravity here).
	h.Step(1.0f);
	REQUIRE(h.world.Get<RigidBodyComponent>(body).body.IsValid());
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, body) == AE_GAMEPLAY_OK);
	CHECK(h.Vector().x > 2.5f);
	CHECK(h.Vector().x < 3.05f);
	CHECK(h.PositionOf(body).x > 2.5f);
	CHECK(h.PositionOf(body).x < 3.1f);

	// Writing a velocity to the baked body replaces it.
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D, body, {0.0f, 0.0f, -4.0f, 0.0f}) == AE_GAMEPLAY_OK);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, body) == AE_GAMEPLAY_OK);
	CHECK(h.Vector().x == doctest::Approx(0.0f).epsilon(0.001));
	CHECK(h.Vector().z == doctest::Approx(-4.0f));

	// An impulse changes velocity by impulse / mass: large enough to dominate any mass.
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D, body, {0.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_OK);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_ADD_IMPULSE_3D, body, {100000.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_OK);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, body) == AE_GAMEPLAY_OK);
	CHECK(h.Vector().x > 0.5f);
	CHECK(h.Vector().y == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("Rust 3D force accelerates only the body it is applied to")
{
	Physics3DHarness h;
	const Entity pushed = h.MakeBody({0.0f, 10.0f, 0.0f}, PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f}, 0.0f);
	const Entity control = h.MakeBody({5.0f, 10.0f, 0.0f}, PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f}, 0.0f);
	h.Step(0.1f);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_ADD_FORCE_3D, pushed, {1000000.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_OK);
	h.Step(PhysicsSystem::kFixedTimestep);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, pushed) == AE_GAMEPLAY_OK);
	CHECK(h.Vector().x > 0.1f);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, control) == AE_GAMEPLAY_OK);
	CHECK(h.Vector().x == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE("Rust 3D physics operations reject missing components, stale entities, non-finite input and a missing system")
{
	Physics3DHarness h;
	const Entity plain = h.world.Create();
	for (const auto operation: {AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D, AE_GAMEPLAY_SERVICE_ADD_FORCE_3D, AE_GAMEPLAY_SERVICE_ADD_IMPULSE_3D, AE_GAMEPLAY_SERVICE_CHARACTER_MOVE, AE_GAMEPLAY_SERVICE_CHARACTER_JUMP,
	             AE_GAMEPLAY_SERVICE_CHARACTER_IS_GROUNDED, AE_GAMEPLAY_SERVICE_SET_ANIMATION_SPEED, AE_GAMEPLAY_SERVICE_GET_ANIMATION_TIME, AE_GAMEPLAY_SERVICE_GET_CURRENT_CLIP})
	{
		CAPTURE(operation);
		CHECK(h.Call(operation, plain) == AE_GAMEPLAY_UNSUPPORTED);
	}

	const Entity body = h.MakeBody({0.0f, 5.0f, 0.0f}, PhysicsMotionType::Dynamic, {0.5f, 0.5f, 0.5f});
	const float inf = std::numeric_limits<float>::infinity();
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D, body, {inf, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_ADD_IMPULSE_3D, body, {0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, body) == AE_GAMEPLAY_OK);
	CHECK(h.Vector() == glm::vec3(0.0f));

	AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, .entity = h.access.Handle(body)};
	h.world.Destroy(body);
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	CHECK(h.api.invoke(h.api.context, &request, &value, {}) == AE_GAMEPLAY_INVALID_HANDLE);

	// Without a PhysicsSystem the physics bits are not advertised and a call is refused.
	Physics3DHarness none(false);
	for (uint32_t operation = AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D; operation <= AE_GAMEPLAY_SERVICE_SPAWN_RAGDOLL; ++operation)
	{
		CAPTURE(operation);
		CHECK((none.api.capabilities & AE_GAMEPLAY_SERVICE_CAP(operation)) == 0);
	}
	for (uint32_t operation = AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP; operation <= AE_GAMEPLAY_SERVICE_GET_CURRENT_CLIP; ++operation)
	{
		CHECK((none.api.capabilities & AE_GAMEPLAY_SERVICE_CAP(operation)) != 0);
	}
	const Entity unsimulated = none.world.Create();
	none.world.Emplace<RigidBodyComponent>(unsimulated);
	CHECK(none.Call(AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D, unsimulated) == AE_GAMEPLAY_UNSUPPORTED);
}

TEST_CASE("Rust 3D raycast reports hit entity, distance, point and normal, and validates its payload")
{
	Physics3DHarness h;
	const Entity floor = h.MakeBody({0.0f, -0.5f, 0.0f}, PhysicsMotionType::Static, {10.0f, 0.5f, 10.0f});
	h.Step(0.1f);
	const auto cast = [&](glm::vec3 origin, glm::vec3 direction, float maxDistance)
	{
		const auto payload = RayPayload(origin, direction, maxDistance);
		return h.Call(AE_GAMEPLAY_SERVICE_RAYCAST_3D, {}, {}, 0, {}, Bytes(payload));
	};

	REQUIRE(cast({1.0f, 5.0f, 2.0f}, {0.0f, -1.0f, 0.0f}, 20.0f) == AE_GAMEPLAY_OK);
	CHECK(h.value.integer == 1);
	CHECK(h.value.vector[0] == doctest::Approx(1.0f).epsilon(0.001));
	CHECK(h.value.vector[1] == doctest::Approx(0.0f).epsilon(0.001));
	CHECK(h.value.vector[2] == doctest::Approx(2.0f).epsilon(0.001));
	CHECK(h.value.vector[3] == doctest::Approx(5.0f).epsilon(0.001));
	CHECK(h.value.entity.id == floor.id);
	CHECK(h.value.entity.world == h.access.Handle(floor).world);
	REQUIRE(h.required == 13);
	float normal[3];
	std::memcpy(normal, h.bytes, sizeof(normal));
	CHECK(normal[0] == doctest::Approx(0.0f).epsilon(0.001));
	CHECK(normal[1] == doctest::Approx(1.0f));
	CHECK(normal[2] == doctest::Approx(0.0f).epsilon(0.001));

	// The direction is normalized: a longer vector does not change the distance.
	REQUIRE(cast({0.0f, 5.0f, 0.0f}, {0.0f, -3.0f, 0.0f}, 20.0f) == AE_GAMEPLAY_OK);
	CHECK(h.value.integer == 1);
	CHECK(h.value.vector[3] == doctest::Approx(5.0f).epsilon(0.001));

	// A miss is a successful, empty result: out of range, pointing away, or beside the floor.
	struct MissCase
	{
		glm::vec3 origin;
		glm::vec3 direction;
		float range;
	};
	for (const auto& [origin, direction, range]: {MissCase{{0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 2.0f}, MissCase{{0.0f, 5.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 20.0f}, MissCase{{50.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f}})
	{
		REQUIRE(cast(origin, direction, range) == AE_GAMEPLAY_OK);
		CHECK(h.value.integer == 0);
		CHECK(h.value.entity.id == 0);
		CHECK(h.required == 13);
	}

	// Malformed requests are refused rather than answered as a miss.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	CHECK(cast({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 20.0f) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(cast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 0.0f) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(cast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, -1.0f) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(cast({nan, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(cast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, nan}, 20.0f) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(cast({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, std::numeric_limits<float>::infinity()) == AE_GAMEPLAY_INVALID_ARGUMENT);
	const auto payload = RayPayload({0.0f, 5.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 20.0f);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_RAYCAST_3D, {}, {}, 0, {}, {Bytes(payload).data, AE_GAMEPLAY_RAYCAST_PAYLOAD_BYTES - 4}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_RAYCAST_3D, {}, {}, 0, {}, {}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	// The 12-byte normal needs a 13-byte result buffer; a short one is reported, not truncated.
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_RAYCAST_3D, {}, {}, 0, {}, Bytes(payload), 4) == AE_GAMEPLAY_BUFFER_TOO_SMALL);
	CHECK(h.required == 13);
}

TEST_CASE("Rust 3D character controller moves, jumps and reports ground state")
{
	Physics3DHarness h;
	h.MakeBody({0.0f, -0.5f, 0.0f}, PhysicsMotionType::Static, {20.0f, 0.5f, 20.0f});
	const Entity character = h.world.Create();
	h.world.Emplace<TransformComponent>(character, TransformComponent{.localToWorld = ComposeTransform({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
	h.world.Emplace<CharacterControllerComponent>(character, CharacterControllerComponent{.radius = 0.3f, .halfHeight = 0.6f});

	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_CHARACTER_IS_GROUNDED, character) == AE_GAMEPLAY_OK);
	CHECK(h.value.type == AE_GAMEPLAY_PROPERTY_BOOL);
	CHECK(h.value.integer == 0);
	h.Step(1.0f);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_CHARACTER_IS_GROUNDED, character) == AE_GAMEPLAY_OK);
	CHECK(h.value.integer == 1);

	const float startX = h.PositionOf(character).x;
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_CHARACTER_MOVE, character, {1.0f, 0.0f, 0.0f, 2.0f}) == AE_GAMEPLAY_OK);
	h.Step(1.0f);
	CHECK(h.PositionOf(character).x - startX > 1.0f);
	CHECK(h.PositionOf(character).x - startX < 2.1f);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_CHARACTER_IS_GROUNDED, character) == AE_GAMEPLAY_OK);
	CHECK(h.value.integer == 1);

	CHECK(h.Call(AE_GAMEPLAY_SERVICE_CHARACTER_JUMP, character, {-1.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_CHARACTER_JUMP, character, {5.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_OK);
	h.Step(0.2f);
	CHECK(h.PositionOf(character).y > 0.3f);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_CHARACTER_IS_GROUNDED, character) == AE_GAMEPLAY_OK);
	CHECK(h.value.integer == 0);
}

TEST_CASE("Rust 3D ragdoll spawn refuses stale entities and unloadable skeletons without changing the entity")
{
	// GltfAsset::LoadFromVfsPath asserts when the VFS was never initialized, so mount a real empty
	// project directory: the failure under test is a genuinely missing file.
	if (io::FileSystem::IsInitialized())
	{
		io::FileSystem::Shutdown();
	}
	const std::filesystem::path root = std::filesystem::temp_directory_path() / "aethercore_rust_ragdoll_missing_skeleton";
	std::error_code ec;
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);
	io::FileSystem::Initialize();
	io::FileSystem::Mount("project", root);
	{
		Physics3DHarness h;
		const Entity source = h.world.Create();
		h.world.Emplace<TransformComponent>(source, TransformComponent{.localToWorld = ComposeTransform({0.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f})});
		CHECK(h.Call(AE_GAMEPLAY_SERVICE_SPAWN_RAGDOLL, source, {}, 0, "project://assets/models/Missing.glb") == AE_GAMEPLAY_INVALID_ARGUMENT);
		CHECK(h.Call(AE_GAMEPLAY_SERVICE_SPAWN_RAGDOLL, source, {}, 0, "") == AE_GAMEPLAY_INVALID_ARGUMENT);
		CHECK_FALSE(h.world.Has<RagdollComponent>(source));
		CHECK_FALSE(h.world.Has<RagdollBoneComponent>(source));
		CHECK(h.world.Has<TransformComponent>(source));
	}
	io::FileSystem::Shutdown();
	std::filesystem::remove_all(root, ec);
}

TEST_CASE("Rust 3D animation operations resolve clips by name or index, cross-fade, and reach spawned child meshes")
{
	Physics3DHarness h(false);
	const auto makeAnimator = [&h]
	{
		const Entity entity = h.world.Create();
		SkinnedMeshComponent smc;
		for (const char* name: {"Idle", "Run", "Jump"})
		{
			assets::GltfAnimation clip;
			clip.name = name;
			smc.pendingExternalAnims.push_back(clip);
		}
		h.world.Emplace<SkinnedMeshComponent>(entity, std::move(smc));
		return entity;
	};
	const Entity actor = makeAnimator();
	auto& smc = h.world.Get<SkinnedMeshComponent>(actor);

	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP, actor, {}, 0, "Run") == AE_GAMEPLAY_OK);
	CHECK(h.value.type == AE_GAMEPLAY_PROPERTY_INT);
	CHECK(h.value.integer == 1);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP, actor, {}, 0, "Nope") == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP, actor, {}, 0, "") == AE_GAMEPLAY_INVALID_ARGUMENT);

	// Cross-fade by name: the previous clip keeps fading out for the requested time.
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, actor, {0.25f, 0.0f, 0.0f, 0.0f}, 0, "Run") == AE_GAMEPLAY_OK);
	CHECK(smc.clipIndex == 1);
	CHECK(smc.fadeClipIndex == 0);
	CHECK(smc.fadeWeight == 1.0f);
	CHECK(smc.fadeRate == doctest::Approx(4.0f));
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_CURRENT_CLIP, actor) == AE_GAMEPLAY_OK);
	CHECK(h.value.integer == 1);

	// By index with no fade switches outright.
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, actor, {0.0f, 0.0f, 0.0f, 0.0f}, 2) == AE_GAMEPLAY_OK);
	CHECK(smc.clipIndex == 2);
	CHECK(smc.fadeWeight == 0.0f);

	// Unknown clips and bad fades change nothing.
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, actor, {0.0f, 0.0f, 0.0f, 0.0f}, 3) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, actor, {0.0f, 0.0f, 0.0f, 0.0f}, -1) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, actor, {0.0f, 0.0f, 0.0f, 0.0f}, 0, "Nope") == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, actor, {-0.5f, 0.0f, 0.0f, 0.0f}, 0) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(smc.clipIndex == 2);

	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_SPEED, actor, {2.0f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_OK);
	CHECK(smc.playbackSpeed == 2.0f);
	smc.animTime = 1.25f;
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_GET_ANIMATION_TIME, actor) == AE_GAMEPLAY_OK);
	CHECK(h.value.type == AE_GAMEPLAY_PROPERTY_FLOAT);
	CHECK(h.value.vector[0] == 1.25f);

	// A spawned model: the controller entity has no mesh itself, its child does.
	const Entity parent = h.world.Create();
	const Entity child = makeAnimator();
	h.world.Emplace<HierarchyComponent>(parent, HierarchyComponent{.children = {child}});
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, parent, {0.0f, 0.0f, 0.0f, 0.0f}, 0, "Jump") == AE_GAMEPLAY_OK);
	CHECK(h.world.Get<SkinnedMeshComponent>(child).clipIndex == 2);
	REQUIRE(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_SPEED, parent, {0.5f, 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_OK);
	CHECK(h.world.Get<SkinnedMeshComponent>(child).playbackSpeed == 0.5f);

	// An entity with no mesh anywhere is refused, as is a non-finite speed.
	const Entity bare = h.world.Create();
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, bare, {0.0f, 0.0f, 0.0f, 0.0f}, 0) == AE_GAMEPLAY_UNSUPPORTED);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP, bare, {}, 0, "Run") == AE_GAMEPLAY_UNSUPPORTED);
	CHECK(h.Call(AE_GAMEPLAY_SERVICE_SET_ANIMATION_SPEED, actor, {std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 0.0f}) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(smc.playbackSpeed == 2.0f);
}

TEST_CASE("Rust 3D clip catalog supplies clip names where the component has no animation database")
{
	struct Catalog final : aether::scripting::gameplay3d::ClipCatalog
	{
		std::uint32_t ClipCount(Entity) const override
		{
			return 2;
		}

		std::string_view ClipName(Entity, std::uint32_t index) const override
		{
			return index == 0 ? "Walk" : "Sprint";
		}

		float ClipDuration(Entity, std::uint32_t) const override
		{
			return 0.0f;
		}
		std::optional<std::uint32_t> FindNode(Entity, std::string_view) const override { return {}; }
		void Remap(Entity, assets::GltfAnimation&) const override {}
		void Compile(World&, Entity) const override {}
	} catalog;
	World world;
	const Entity actor = world.Create();
	SkinnedMeshComponent smc;
	assets::GltfAnimation own;
	own.name = "Wave";
	smc.pendingExternalAnims.push_back(own);
	world.Emplace<SkinnedMeshComponent>(actor, std::move(smc));
	AetherGameplayPropertyValue value{.structSize = sizeof(value)};
	const auto run = [&](uint32_t operation, const std::string& name, int64_t index)
	{
		AetherGameplayServiceRequest request{.structSize = sizeof(request), .operation = operation, .entity = {0, actor.id, 0}, .integer = index, .text = {name.data(), static_cast<uint32_t>(name.size())}};
		value = {.structSize = sizeof(value)};
		return aether::scripting::gameplay3d::ExecuteGameplay3DOperation(world, nullptr, request, value, {}, &catalog).status;
	};
	REQUIRE(run(AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP, "Sprint", 0) == AE_GAMEPLAY_OK);
	CHECK(value.integer == 1);
	REQUIRE(run(AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP, "Wave", 0) == AE_GAMEPLAY_OK);
	CHECK(value.integer == 2);
	REQUIRE(run(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, "Sprint", 0) == AE_GAMEPLAY_OK);
	CHECK(world.Get<SkinnedMeshComponent>(actor).clipIndex == 1);
	CHECK(run(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, "", 3) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(run(AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP, "", 2) == AE_GAMEPLAY_OK);
}
