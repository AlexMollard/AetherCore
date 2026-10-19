#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpGameplayHost.hpp"
#include "scripting/GameplayHost.hpp"
#include "scripting/ManagedInterop.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/ServiceContainer.hpp"
#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

namespace
{
	using namespace aether;
	using namespace aether::app;
	using namespace aether::app::scripting;
	using ManagedApi = aether::scripting::ManagedScriptApi;
	using ManagedValue = aether::scripting::PropertyValue;

	struct ManagedRunnerState
	{
		struct Instance
		{
			std::string type;
			Entity entity;
			std::array<ScriptPropertyValue, 4> properties;
		};

		std::map<std::uint64_t, Instance> instances;
		std::vector<std::string> calls;
		std::function<void()> onAttach;
		std::uint64_t nextHandle = 1;
		bool contextsValid = true;
		bool missingAvailable = false;
	};

	ManagedRunnerState* g_runnerState = nullptr;

	std::int32_t CopyRunnerText(std::string_view value, char* buffer, std::int32_t capacity)
	{
		if (buffer == nullptr || capacity <= static_cast<std::int32_t>(value.size()))
		{
			return 0;
		}
		std::memcpy(buffer, value.data(), value.size());
		buffer[value.size()] = '\0';
		return static_cast<std::int32_t>(value.size());
	}

	ManagedApi RunnerApi()
	{
		ManagedApi api;
		api.GetScriptTypeCount = []() -> std::int32_t { return 3; };
		api.GetScriptTypeName = [](std::int32_t index, char* buffer, std::int32_t capacity)
		{
			return CopyRunnerText(index == 0 ? "First" : index == 1 ? "Second" : "Missing", buffer, capacity);
		};
		api.GetPropertyCount = [](const char*) -> std::int32_t { return 4; };
		api.GetPropertyInfo = [](const char*, std::int32_t index, char* buffer, std::int32_t capacity, std::int32_t* type)
		{
			constexpr std::array<std::string_view, 4> names{"Speed", "Target", "Body", "Label"};
			constexpr std::array<std::int32_t, 4> types{1, 7, 8, 5};
			if (index < 0 || index >= static_cast<std::int32_t>(names.size()))
			{
				return std::int32_t{0};
			}
			*type = types[static_cast<std::size_t>(index)];
			return CopyRunnerText(names[static_cast<std::size_t>(index)], buffer, capacity);
		};
		api.CreateInstance = [](const char* type, std::uint32_t entityId) -> std::uint64_t
		{
			auto& state = *g_runnerState;
			state.contextsValid &= g_activeContext != nullptr && g_activeContext->world != nullptr;
			if (std::string_view(type) == "Missing" && !state.missingAvailable)
			{
				state.calls.emplace_back("missing");
				return 0;
			}
			const auto handle = state.nextHandle++;
			auto& instance = state.instances[handle];
			instance.type = type;
			instance.entity = Entity{entityId};
			instance.properties[0].type = ScriptPropertyValue::Type::Float;
			instance.properties[1].type = ScriptPropertyValue::Type::Entity;
			instance.properties[2].type = ScriptPropertyValue::Type::Component;
			instance.properties[2].str = "Rigid Body";
			instance.properties[3].type = ScriptPropertyValue::Type::String;
			state.calls.emplace_back("create:" + std::to_string(handle));
			return handle;
		};
		api.DestroyInstance = [](std::uint64_t handle)
		{
			g_runnerState->contextsValid &= g_activeContext != nullptr;
			g_runnerState->calls.emplace_back("destroy:" + std::to_string(handle));
			g_runnerState->instances.erase(handle);
		};
		api.InvokeAttach = [](std::uint64_t handle)
		{
			g_runnerState->contextsValid &= g_activeContext != nullptr;
			g_runnerState->calls.emplace_back("attach:" + std::to_string(handle));
			if (g_runnerState->onAttach)
			{
				g_runnerState->onAttach();
			}
		};
		api.InvokeUpdate = [](std::uint64_t handle, float)
		{
			g_runnerState->contextsValid &= g_activeContext != nullptr;
			g_runnerState->calls.emplace_back("update:" + std::to_string(handle));
		};
		api.InvokeDetach = [](std::uint64_t handle)
		{
			g_runnerState->contextsValid &= g_activeContext != nullptr;
			g_runnerState->calls.emplace_back("detach:" + std::to_string(handle));
		};
		api.InvokeOwnershipChanged = [](std::uint64_t handle, std::uint32_t, std::int32_t)
		{
			g_runnerState->contextsValid &= g_activeContext != nullptr;
			g_runnerState->calls.emplace_back("ownership:" + std::to_string(handle));
		};
		api.GetProperty = [](std::uint64_t handle, std::int32_t index, ManagedValue* out) -> std::int32_t
		{
			const auto& property = g_runnerState->instances.at(handle).properties.at(static_cast<std::size_t>(index));
			out->type = static_cast<aether::scripting::PropertyType>(property.type);
			std::copy_n(property.f4, 4, out->f4);
			out->i64 = property.i64;
			out->str = property.str.data();
			out->reserved = static_cast<std::int32_t>(property.str.size());
			return 1;
		};
		api.SetProperty = [](std::uint64_t handle, std::int32_t index, const ManagedValue* value) -> std::int32_t
		{
			auto& property = g_runnerState->instances.at(handle).properties.at(static_cast<std::size_t>(index));
			property.type = static_cast<ScriptPropertyValue::Type>(value->type);
			std::copy_n(value->f4, 4, property.f4);
			property.i64 = value->i64;
			property.str.assign(value->str == nullptr ? "" : value->str, value->reserved >= 0 ? static_cast<std::size_t>(value->reserved) : std::strlen(value->str));
			return 1;
		};
		api.GetDefaultProperty = [](const char*, std::int32_t index, ManagedValue* out) -> std::int32_t
		{
			constexpr std::array<std::int32_t, 4> types{1, 7, 8, 5};
			*out = {};
			out->type = static_cast<aether::scripting::PropertyType>(types.at(static_cast<std::size_t>(index)));
			out->str = index == 2 ? "Rigid Body" : "";
			out->reserved = static_cast<std::int32_t>(std::strlen(out->str));
			return 1;
		};
		return api;
	}

	struct RunnerFixture
	{
		ManagedRunnerState state;
		ManagedApi api = RunnerApi();
		CSharpGameplayHost host;
		ServiceContainer services;
		World world;
		SceneContext context;
		ScriptComponentSystem* runner = nullptr;

		RunnerFixture()
		{
			g_runnerState = &state;
			REQUIRE(host.Reset(&api, 1).has_value());
			services.Register<GameplayHost>(host.Gameplay());
			context.world = &world;
			context.services = &services;
			services.Register(context);
			auto system = std::make_unique<ScriptComponentSystem>(services);
			runner = system.get();
			world.RegisterSystem(std::move(system));
		}

		~RunnerFixture()
		{
			runner->Invalidate(world);
			g_runnerState = nullptr;
		}

		Entity Scripted(std::vector<ScriptEntry> entries, std::string name = "Scripted")
		{
			const Entity entity = world.Create();
			world.Emplace<NameComponent>(entity, NameComponent{.name = std::move(name)});
			world.Emplace<TransformComponent>(entity);
			world.Emplace<ScriptComponent>(entity, ScriptComponent{.scripts = std::move(entries)});
			return entity;
		}
	};
}

TEST_CASE("Shared gameplay runner preserves ordered sibling lifecycle and reload invalidation")
{
	RunnerFixture fixture;
	const Entity entity = fixture.Scripted({ScriptEntry{.path = "First"}, ScriptEntry{.path = "Second"}});
	fixture.runner->Update(fixture.world, 0.25f);
	CHECK(fixture.state.calls == std::vector<std::string>{"create:1", "attach:1", "ownership:1", "update:1", "create:2", "attach:2", "ownership:2", "update:2"});
	CHECK(fixture.runner->GetInstanceHandle(entity.id, 0) == 1);
	CHECK(fixture.runner->GetInstanceHandle(entity.id, 1) == 2);
	CHECK(g_activeContext == nullptr);
	fixture.state.calls.clear();
	fixture.runner->Update(fixture.world, 0.25f);
	CHECK(fixture.state.calls == std::vector<std::string>{"update:1", "update:2"});
	fixture.state.calls.clear();
	fixture.runner->Invalidate(fixture.world);
	CHECK(fixture.state.instances.empty());
	CHECK_FALSE(fixture.world.Get<ScriptComponent>(entity).scripts[0].attached);
	CHECK_FALSE(fixture.world.Get<ScriptComponent>(entity).scripts[1].attached);
	for (const std::uint64_t handle : {std::uint64_t{1}, std::uint64_t{2}})
	{
		const auto detach = std::ranges::find(fixture.state.calls, "detach:" + std::to_string(handle));
		const auto destroy = std::ranges::find(fixture.state.calls, "destroy:" + std::to_string(handle));
		REQUIRE(detach != fixture.state.calls.end());
		REQUIRE(destroy != fixture.state.calls.end());
		CHECK(detach < destroy);
	}
	fixture.runner->Update(fixture.world, 0.25f);
	CHECK(fixture.runner->GetInstanceHandle(entity.id, 0) == 3);
	CHECK(fixture.runner->GetInstanceHandle(entity.id, 1) == 4);
	CHECK(fixture.state.contextsValid);
}

TEST_CASE("Shared gameplay runner tolerates an attach callback growing component and script storage")
{
	RunnerFixture fixture;
	const Entity entity = fixture.Scripted({ScriptEntry{.path = "First"}, ScriptEntry{.path = "Second"}});
	bool grew = false;
	fixture.state.onAttach = [&]
	{
		if (std::exchange(grew, true))
		{
			return;
		}
		for (int i = 0; i < 2048; ++i)
		{
			const Entity extra = fixture.world.Create();
			fixture.world.Emplace<ScriptComponent>(extra);
		}
		fixture.world.Get<ScriptComponent>(entity).scripts.push_back(ScriptEntry{.path = "First"});
	};
	fixture.runner->Update(fixture.world, 0.1f);
	CHECK(fixture.state.instances.size() == 3);
	CHECK(fixture.runner->GetInstanceHandle(entity.id, 2) == 3);
	CHECK(std::ranges::count(fixture.state.calls, "attach:1") == 1);
	CHECK(std::ranges::count(fixture.state.calls, "update:3") == 1);
	CHECK(fixture.state.contextsValid);
}

TEST_CASE("Shared gameplay runner preserves missing script fields and retries only after invalidation")
{
	RunnerFixture fixture;
	ScriptPropertyValue authored;
	authored.type = ScriptPropertyValue::Type::Float;
	authored.f4[0] = 7.5f;
	const Entity entity = fixture.Scripted({ScriptEntry{.path = "Missing", .properties = {{"Speed", authored}}}});
	fixture.runner->Update(fixture.world, 0.1f);
	fixture.runner->Update(fixture.world, 0.1f);
	CHECK(std::ranges::count(fixture.state.calls, "missing") == 1);
	CHECK(fixture.runner->GetInstanceHandle(entity.id, 0) == 0);
	CHECK(fixture.world.Get<ScriptComponent>(entity).scripts[0].properties.at("Speed").f4[0] == doctest::Approx(7.5f));
	fixture.state.missingAvailable = true;
	fixture.runner->Invalidate(fixture.world);
	fixture.runner->Update(fixture.world, 0.1f);
	const auto handle = fixture.runner->GetInstanceHandle(entity.id, 0);
	REQUIRE(handle != 0);
	CHECK(fixture.state.instances.at(handle).properties[0].f4[0] == doctest::Approx(7.5f));
}

TEST_CASE("Shared gameplay runner refreshes live fields for scene capture and restores references")
{
	RunnerFixture fixture;
	FakeSlotSink slotSink;
	FakeTextureSink textureSink;
	TextureRegistry textures(textureSink);
	MaterialRegistry materials(slotSink, textures);
	const Entity target = fixture.world.Create();
	fixture.world.Emplace<NameComponent>(target, NameComponent{.name = "Target"});
	fixture.world.Emplace<TransformComponent>(target);
	const Entity entity = fixture.Scripted({ScriptEntry{.path = "First"}});
	fixture.runner->Update(fixture.world, 0.1f);
	const auto handle = fixture.runner->GetInstanceHandle(entity.id, 0);
	REQUIRE(handle != 0);
	auto& properties = fixture.state.instances.at(handle).properties;
	properties[0].f4[0] = 12.5f;
	properties[1].i64 = target.id;
	properties[2].i64 = target.id;
	properties[3].str = "live string";
	const auto parsed = aether::app::scene::ParseToml(aether::app::scene::WriteToml(aether::app::scene::CaptureScene(fixture.world, materials, textures)));
	REQUIRE(parsed.has_value());
	const auto& cached = fixture.world.Get<ScriptComponent>(entity).scripts[0].properties;
	CHECK(cached.at("Speed").f4[0] == doctest::Approx(12.5f));
	CHECK(cached.at("Label").str == "live string");
	World restored;
	const Entity padding = restored.Create();
	restored.Emplace<NameComponent>(padding, NameComponent{.name = "Padding"});
	const auto created = aether::app::scene::ApplyScene(*parsed, restored, {});
	Entity restoredTarget;
	Entity restoredScripted;
	for (const Entity candidate : created)
	{
		if (restored.Get<NameComponent>(candidate).name == "Target")
		{
			restoredTarget = candidate;
		}
		if (restored.Get<NameComponent>(candidate).name == "Scripted")
		{
			restoredScripted = candidate;
		}
	}
	REQUIRE(restoredTarget.IsValid());
	REQUIRE(restoredScripted.IsValid());
	const auto& saved = restored.Get<ScriptComponent>(restoredScripted).scripts[0].properties;
	CHECK(saved.at("Speed").f4[0] == doctest::Approx(12.5f));
	CHECK(saved.at("Target").i64 == restoredTarget.id);
	CHECK(saved.at("Body").i64 == restoredTarget.id);
	CHECK(saved.at("Body").str == "Rigid Body");
	CHECK(saved.at("Label").str == "live string");
}
