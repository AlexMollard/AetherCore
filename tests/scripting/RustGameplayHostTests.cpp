#include <doctest/doctest.h>

#include <cstdlib>

#include "scene/World.hpp"
#include "scene/Hierarchy.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/RustGameplayHost.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/ServiceContainer.hpp"

namespace
{
	using namespace aether;
	using namespace aether::app::scripting;

	struct NativeFixture
	{
		static inline AetherGameplayEntity entity{};
		static inline int destroyed = 0;
		static inline int detached = 0;
		static inline int updates = 0;

		static AetherGameplayModuleApi Api()
		{
			AetherGameplayModuleApi api{};
			api.structSize = sizeof(api);
			api.abiMajor = AE_GAMEPLAY_ABI_MAJOR;
			api.initialize = [](uint64_t, const AetherGameplayEngineApi*)
			{
				return AE_GAMEPLAY_OK;
			};
			api.shutdown = [](uint64_t)
			{
				return AE_GAMEPLAY_OK;
			};
			api.getTypeCount = [](uint64_t, uint32_t* count)
			{
				*count = 1;
				return AE_GAMEPLAY_OK;
			};
			api.getTypeInfo = [](uint64_t, uint32_t, AetherGameplayTypeInfo* info)
			{
				info->name = {"Probe", 5};
				info->propertyCount = 1;
				return AE_GAMEPLAY_OK;
			};
			api.getPropertyInfo = [](uint64_t, uint32_t, uint32_t, AetherGameplayPropertyInfo* info)
			{
				info->name = {"Target", 6};
				info->type = AE_GAMEPLAY_PROPERTY_ENTITY;
				return AE_GAMEPLAY_OK;
			};
			api.create = [](uint64_t, uint32_t, AetherGameplayEntity value, uint64_t* out)
			{
				entity = value;
				*out = 1;
				return AE_GAMEPLAY_OK;
			};
			api.destroy = [](uint64_t, uint64_t)
			{
				++destroyed;
				return AE_GAMEPLAY_OK;
			};
			api.attach = [](uint64_t, uint64_t)
			{
				return AE_GAMEPLAY_OK;
			};
			api.detach = [](uint64_t, uint64_t)
			{
				++detached;
				return AE_GAMEPLAY_OK;
			};
			api.update = [](uint64_t, uint64_t, float)
			{
				++updates;
				return AE_GAMEPLAY_OK;
			};
			api.getProperty = [](uint64_t, uint64_t, uint32_t, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer)
			{
				out->type = AE_GAMEPLAY_PROPERTY_ENTITY;
				out->entity = entity;
				*buffer.required = 0;
				return AE_GAMEPLAY_OK;
			};
			api.getDefaultProperty = [](uint64_t, uint32_t, uint32_t, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer)
			{
				return Api().getProperty(0, 0, 0, out, buffer);
			};
			api.setProperty = [](uint64_t, uint64_t, uint32_t, const AetherGameplayPropertyValue* value, AetherGameplayUtf8)
			{
				entity = value->entity;
				return AE_GAMEPLAY_OK;
			};
			api.captureStreamingState = [](uint64_t, AetherGameplayInstance, AetherGameplayUtf8Buffer buffer)
			{
				return aether::scripting::CopyGameplayUtf8({nullptr, 0}, buffer);
			};
			api.restoreStreamingState = [](uint64_t, AetherGameplayInstance, AetherGameplayUtf8 state)
			{
				return aether::scripting::IsGameplayUtf8(state) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_INVALID_ARGUMENT;
			};
			return api;
		}
	};
} // namespace

TEST_CASE("Rust checked host scopes entity references and destroys native allocations before clearing callbacks")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	GameplayHost host;
	host.SetAccess(&access);
	NativeFixture::destroyed = NativeFixture::detached = NativeFixture::updates = 0;
	REQUIRE(host.Reset(NativeFixture::Api(), "rust", 1));
	const auto entity = world.Create();
	const auto first = host.CreateInstance("Probe", entity.id);
	REQUIRE(first);
	CHECK(NativeFixture::entity.world == access.Handle(entity).world);
	CHECK(NativeFixture::entity.id == entity.id);
	REQUIRE(host.Attach(*first));
	ScriptPropertyValue value;
	value.type = ScriptPropertyValue::Type::Entity;
	value.i64 = entity.id;
	host.SetPropertyValue(*first, 0, value);
	CHECK(NativeFixture::entity.world == access.Handle(entity).world);
	value.i64 = 0;
	host.SetPropertyValue(*first, 0, value);
	CHECK(NativeFixture::entity.world == 0);
	CHECK(NativeFixture::entity.id == 0);
	ScriptPropertyValue cleared;
	REQUIRE(host.GetPropertyValue(*first, 0, cleared));
	CHECK(cleared.i64 == 0);
	value.i64 = entity.id;
	host.SetPropertyValue(*first, 0, value);
	{
		aether::scripting::GameplayAccess::Scope scope(access, aether::scripting::GameplayAccess::ScopeKind::Callback);
		CHECK_FALSE(host.Update(*first, 1));
	}
	CHECK(NativeFixture::updates == 0);
	host.Clear(2);
	CHECK(NativeFixture::detached == 1);
	CHECK(NativeFixture::destroyed == 1);
	REQUIRE(host.Reset(NativeFixture::Api(), "rust", 3));
	const auto second = host.CreateInstance("Probe", entity.id);
	REQUIRE(second);
	CHECK(*second != *first);
	CHECK_FALSE(host.Update(*first, 1));
	CHECK(host.Update(*second, 1));
	CHECK(NativeFixture::updates == 1);
	NativeFixture::entity.world += 1;
	value.str = "retained";
	CHECK_FALSE(host.GetPropertyValue(*second, 0, value));
	CHECK(value.str == "retained");
	host.Clear(4);
}

TEST_CASE("Rust native loader reports missing and invalid libraries without retaining callbacks")
{
	World world;
	RustGameplayHost host;
	CHECK_FALSE(host.Load("missing-aether-gameplay-library.dll", world));
	REQUIRE_FALSE(host.Diagnostics().empty());
	CHECK(host.Diagnostics().back().code == "RUST_LIBRARY_MISSING");
	CHECK_FALSE(host.Gameplay().IsAvailable());
	CHECK(host.Gameplay().GetScriptTypeNames().empty());
	host.Unload();
	CHECK_FALSE(host.Gameplay().Update(1, 0));
}

TEST_CASE("Rust native loader executes an actual SDK module and rejects handles after unload")
{
	const char* module = std::getenv("AETHER_RUST_TEST_MODULE");
	if (module == nullptr || *module == '\0')
	{
		MESSAGE("Set AETHER_RUST_TEST_MODULE to run the native Rust SDK fixture");
		return;
	}
	World world;
	RustGameplayHost host;
	REQUIRE(host.Load(module, world));
	CHECK(host.Gameplay().Language() == "rust");
	const auto types = host.Gameplay().GetScriptTypeNames();
	REQUIRE_FALSE(types.empty());
	const auto entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	const auto created = host.Gameplay().CreateInstance("Probe", entity.id);
	REQUIRE(created);
	CHECK(host.Gameplay().Attach(*created));
	CHECK(host.Gameplay().Update(*created, 0.25f));
	ScriptPropertyValue ticks;
	REQUIRE(host.Gameplay().GetPropertyValue(*created, host.Gameplay().FindPropertyIndex("Probe", "ticks"), ticks));
	CHECK(ticks.i64 == 1);
	CHECK(world.TryGet<TransformComponent>(entity)->localToWorld[3].x == doctest::Approx(0.5f));
	ScriptPropertyValue label;
	REQUIRE(host.Gameplay().GetDefaultPropertyValue("Probe", host.Gameplay().FindPropertyIndex("Probe", "label"), label));
	CHECK(label.str == "h\xC3\xA9llo Rust");
	label.str = std::string("left\0right", 10);
	host.Gameplay().SetPropertyValue(*created, host.Gameplay().FindPropertyIndex("Probe", "label"), label);
	ScriptPropertyValue copied;
	REQUIRE(host.Gameplay().GetPropertyValue(*created, host.Gameplay().FindPropertyIndex("Probe", "label"), copied));
	CHECK(copied.str == label.str);
	ScriptPropertyValue target;
	target.type = ScriptPropertyValue::Type::Entity;
	target.i64 = entity.id;
	host.Gameplay().SetPropertyValue(*created, host.Gameplay().FindPropertyIndex("Probe", "target"), target);
	REQUIRE(host.Gameplay().GetPropertyValue(*created, host.Gameplay().FindPropertyIndex("Probe", "target"), copied));
	CHECK(copied.i64 == entity.id);
	const int componentIndex = host.Gameplay().FindPropertyIndex("Probe", "component");
	ScriptPropertyValue component;
	REQUIRE(host.Gameplay().GetDefaultPropertyValue("Probe", componentIndex, component));
	CHECK(component.type == ScriptPropertyValue::Type::Component);
	CHECK(component.str == "Rigid Body 2D");
	component.i64 = entity.id;
	host.Gameplay().SetPropertyValue(*created, componentIndex, component);
	REQUIRE(host.Gameplay().GetPropertyValue(*created, componentIndex, copied));
	CHECK(copied.i64 == entity.id);
	CHECK(copied.str == "Rigid Body 2D");
	CHECK(host.Gameplay().DispatchPhysics(*created, entity.id, 0));
	REQUIRE(host.Gameplay().GetPropertyValue(*created, host.Gameplay().FindPropertyIndex("Probe", "events"), copied));
	CHECK(copied.i64 == 1);
	CHECK_FALSE(host.Diagnostics().empty());
	host.Unload();
	CHECK_FALSE(host.Gameplay().IsAvailable());
	CHECK_FALSE(host.Gameplay().Update(*created, 0.25f));
	REQUIRE(host.Load(module, world));
	CHECK_FALSE(host.Gameplay().Update(*created, 0.25f));
	host.Unload();
}

TEST_CASE("Rust native loader distinguishes an incompatible SDK ABI")
{
	const char* module = std::getenv("AETHER_RUST_WRONG_ABI_MODULE");
	if (module == nullptr || *module == '\0')
	{
		MESSAGE("Set AETHER_RUST_WRONG_ABI_MODULE to run the incompatible Rust ABI fixture");
		return;
	}
	World world;
	RustGameplayHost host;
	CHECK_FALSE(host.Load(module, world));
	REQUIRE_FALSE(host.Diagnostics().empty());
	CHECK(host.Diagnostics().back().code == "RUST_ABI_MISMATCH");
	CHECK_FALSE(host.Gameplay().IsAvailable());
}

TEST_CASE("Rust runner disables only the failed instance and preserves authored fields")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	GameplayHost host;
	host.SetAccess(&access);
	auto api = NativeFixture::Api();
	api.create = [](uint64_t, uint32_t, AetherGameplayEntity value, uint64_t* out)
	{
		*out = value.id;
		return AE_GAMEPLAY_OK;
	};
	static uint64_t failing;
	static int healthyUpdates;
	static int failingUpdates;
	healthyUpdates = failingUpdates = 0;
	api.update = [](uint64_t, uint64_t handle, float)
	{
		if (handle == failing)
		{
			++failingUpdates;
			return AE_GAMEPLAY_SCRIPT_ERROR;
		}
		++healthyUpdates;
		return AE_GAMEPLAY_OK;
	};
	REQUIRE(host.Reset(api, "rust", 1));
	ServiceContainer services;
	services.Register<GameplayHost>(host);
	SceneContext context;
	context.world = &world;
	context.services = &services;
	services.Register(context);
	aether::app::ScriptComponentSystem runner(services);
	const auto bad = world.Create();
	const auto good = world.Create();
	failing = bad.id;
	ScriptPropertyValue authored;
	authored.type = ScriptPropertyValue::Type::Entity;
	authored.i64 = good.id;
	world.Emplace<ScriptComponent>(bad, ScriptComponent{.scripts = {ScriptEntry{.path = "Probe", .properties = {{"Target", authored}}}}});
	world.Emplace<ScriptComponent>(good, ScriptComponent{.scripts = {ScriptEntry{.path = "Probe"}}});
	runner.Update(world, 0.25f);
	CHECK(runner.GetInstanceHandle(bad.id, 0) == 0);
	CHECK_FALSE(world.Get<ScriptComponent>(bad).scripts[0].attached);
	CHECK(world.Get<ScriptComponent>(bad).scripts[0].properties.at("Target").i64 == good.id);
	CHECK(runner.GetInstanceHandle(good.id, 0) != 0);
	runner.Update(world, 0.25f);
	CHECK(failingUpdates == 1);
	CHECK(healthyUpdates == 2);
	runner.Invalidate(world);
	host.Clear(2);
}

TEST_CASE("Rust deferred entity destruction detaches scripts in the subtree while owners remain alive")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	GameplayHost host;
	host.SetAccess(&access);
	static AetherGameplayEngineApi engine;
	static uint64_t worldToken;
	static int aliveDetaches;
	engine = access.EngineApi();
	worldToken = access.Handle(Entity{}).world;
	aliveDetaches = 0;
	auto api = NativeFixture::Api();
	api.create = [](uint64_t, uint32_t, AetherGameplayEntity entity, uint64_t* out)
	{
		*out = entity.id;
		return AE_GAMEPLAY_OK;
	};
	api.detach = [](uint64_t, uint64_t handle)
	{
		uint32_t alive = 0;
		const auto status = engine.entityValid(engine.context, {worldToken, static_cast<uint32_t>(handle), 0}, &alive);
		if (status == AE_GAMEPLAY_OK && alive != 0)
		{
			++aliveDetaches;
		}
		return status;
	};
	REQUIRE(host.Reset(api, "rust", 1));
	ServiceContainer services;
	services.Register<GameplayHost>(host);
	SceneContext context;
	context.world = &world;
	context.services = &services;
	services.Register(context);
	aether::app::ScriptComponentSystem runner(services);
	const auto parent = world.Create();
	const auto child = world.Create();
	REQUIRE(aether::ecs::SetParent(world, child, parent));
	for (const auto entity: {parent, child})
	{
		world.Emplace<ScriptComponent>(entity, ScriptComponent{.scripts = {ScriptEntry{.path = "Probe"}}});
	}
	context.pendingDestroys.push_back(parent);
	runner.Update(world, 0.25f);
	CHECK(aliveDetaches == 2);
	CHECK(runner.GetInstanceHandle(parent.id, 0) == 0);
	CHECK(runner.GetInstanceHandle(child.id, 0) == 0);
	CHECK_FALSE(world.GetRegistry().valid(World::ToEntt(parent)));
	CHECK_FALSE(world.GetRegistry().valid(World::ToEntt(child)));
	runner.Invalidate(world);
	host.Clear(2);
}

TEST_CASE("Rust component reference reads retain their declared component type")
{
	World world;
	aether::scripting::GameplayAccess access(world);
	GameplayHost host;
	host.SetAccess(&access);
	auto api = NativeFixture::Api();
	api.getPropertyInfo = [](uint64_t, uint32_t, uint32_t, AetherGameplayPropertyInfo* info)
	{
		info->name = {"Body", 4};
		info->type = AE_GAMEPLAY_PROPERTY_COMPONENT;
		info->componentType = {"Rigid Body 2D", 13};
		return AE_GAMEPLAY_OK;
	};
	api.getProperty = [](uint64_t, uint64_t, uint32_t, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer)
	{
		out->type = AE_GAMEPLAY_PROPERTY_COMPONENT;
		out->entity = NativeFixture::entity;
		*buffer.required = 0;
		return AE_GAMEPLAY_OK;
	};
	api.getDefaultProperty = [](uint64_t, uint32_t, uint32_t, AetherGameplayPropertyValue* out, AetherGameplayUtf8Buffer buffer)
	{
		out->type = AE_GAMEPLAY_PROPERTY_COMPONENT;
		out->entity = {};
		*buffer.required = 0;
		return AE_GAMEPLAY_OK;
	};
	REQUIRE(host.Reset(api, "rust", 1));
	const auto entity = world.Create();
	const auto instance = host.CreateInstance("Probe", entity.id);
	REQUIRE(instance);
	ScriptPropertyValue value;
	REQUIRE(host.GetPropertyValue(*instance, 0, value));
	CHECK(value.str == "Rigid Body 2D");
	CHECK(value.i64 == entity.id);
	REQUIRE(host.GetDefaultPropertyValue("Probe", 0, value));
	CHECK(value.str == "Rigid Body 2D");
	CHECK(value.i64 == 0);
	host.Clear(2);
}
