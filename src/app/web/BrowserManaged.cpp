#include "BrowserManaged.hpp"

#include <cstring>
#include <emscripten.h>
#include "WebApplication.hpp"
#include "scripting/CSharpGameplayHost.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayServices.h"
#include "scene/World.hpp"
#include "physics2d/Physics2DComponents.hpp"

namespace
{
	aether::app::scripting::CSharpGameplayHost s_host;
	aether::scripting::ManagedScriptApi s_api{};
	AetherGameplayModuleApi s_raw{};
	AetherGameplayEngineApi s_engine{};
	AetherGameplayServicesApi s_services{};
	aether::World* s_world = nullptr;
	aether::scripting::GameplayAccess* s_access = nullptr;

	void Error(const char* text)
	{
		s_host.ReportManagedError();
		EM_ASM({
			aetherRuntime.status = 'failed';
			aetherRuntime.diagnostics.push({code: 'BROWSER_CSHARP', message: UTF8ToString($0)});
		}, text);
	}

	AetherGameplayUtf8 View(const char* value)
	{
		return {value, value ? static_cast<std::uint32_t>(std::strlen(value)) : 0};
	}

	AetherGameplayEntity Handle(std::uint32_t id)
	{
		return s_access && id != 0 ? s_access->Handle(aether::Entity{id}) : AetherGameplayEntity{};
	}

	bool Valid(std::uint32_t id)
	{
		std::uint32_t valid = 0;
		return s_engine.entityValid && s_engine.entityValid(s_engine.context, Handle(id), &valid) == AE_GAMEPLAY_OK && valid;
	}

	AetherGameplayStatus Invoke(AetherGameplayServiceRequest request, AetherGameplayPropertyValue& value, AetherGameplayUtf8Buffer buffer = {})
	{
		request.structSize = sizeof(request);
		value = {};
		value.structSize = sizeof(value);
		if (!s_services.invoke)
		{
			Error("Browser gameplay services are not initialized");
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		return s_services.invoke(s_services.context, &request, &value, buffer);
	}

	void Require(AetherGameplayStatus status)
	{
		if (status != AE_GAMEPLAY_OK)
		{
			Error("C# browser SDK operation failed; verify the entity and supported Web profile");
		}
	}
}

namespace aether::app::web
{
	void BindManagedBrowserWorld(World* world, aether::scripting::GameplayAccess* access)
	{
		s_world = world;
		s_access = access;
		if (!world)
		{
			s_engine = {};
			s_services = {};
		}
	}
}

extern "C"
{
	EMSCRIPTEN_KEEPALIVE int aether_managed_start(int (*bootstrap)(aether::scripting::NativeHostCallbacks*, int, aether::scripting::ManagedScriptApi*, int))
	{
		EM_ASM({ globalThis.Module = Module; });
		aether::scripting::NativeHostCallbacks callbacks{
			[](std::int32_t, const char* text) { EM_ASM({ console.info(UTF8ToString($0)); }, text); },
			[](std::int32_t, const char* text, const char*, std::int32_t) { EM_ASM({ console.info(UTF8ToString($0)); }, text); },
			Error};
		if (!bootstrap || bootstrap(&callbacks, sizeof(callbacks), &s_api, sizeof(s_api)) != 0 || s_api.LoadScripts("AetherGame") < 0)
		{
			Error("C# browser bootstrap or prebuilt assembly registration failed");
			return 1;
		}
		if (!s_host.Reset(&s_api, 1))
		{
			Error("C# browser script metadata registration failed");
			return 1;
		}
		s_raw = s_host.ModuleApi();
		return RunBrowserApplication();
	}

	AetherGameplayStatus aether_gameplay_query(std::uint32_t major, std::uint32_t size, AetherGameplayModuleApi* output)
	{
		if (major != AE_GAMEPLAY_ABI_MAJOR || size != sizeof(*output) || !output || !s_raw.create)
		{
			return AE_GAMEPLAY_ABI_MISMATCH;
		}
		*output = s_raw;
		output->initialize = [](std::uint64_t, const AetherGameplayEngineApi* engine)
		{
			if (!engine || engine->structSize != sizeof(*engine) || engine->abiMajor != AE_GAMEPLAY_ABI_MAJOR)
			{
				return AE_GAMEPLAY_ABI_MISMATCH;
			}
			s_engine = *engine;
			return AE_GAMEPLAY_OK;
		};
		output->create = [](std::uint64_t context, std::uint32_t type, AetherGameplayEntity entity, AetherGameplayInstance* instance)
		{
			if (!s_access || entity.world != Handle(entity.id).world || !Valid(entity.id))
			{
				return AE_GAMEPLAY_INVALID_HANDLE;
			}
			entity.world = 0;
			return s_raw.create(context, type, entity, instance);
		};
		output->getProperty = [](std::uint64_t context, std::uint64_t instance, std::uint32_t index, AetherGameplayPropertyValue* value, AetherGameplayUtf8Buffer buffer)
		{
			const auto status = s_raw.getProperty(context, instance, index, value, buffer);
			if (status == AE_GAMEPLAY_OK && (value->type == AE_GAMEPLAY_PROPERTY_ENTITY || value->type == AE_GAMEPLAY_PROPERTY_COMPONENT))
			{
				value->entity = Handle(value->entity.id);
			}
			return status;
		};
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus aether_gameplay_bind_services(std::uint32_t major, std::uint32_t size, const AetherGameplayServicesApi* api)
	{
		if (major != AE_GAMEPLAY_SERVICES_ABI_MAJOR || size != sizeof(*api) || !api || !api->invoke)
		{
			return AE_GAMEPLAY_ABI_MISMATCH;
		}
		s_services = *api;
		return AE_GAMEPLAY_OK;
	}

	int aether_entity_valid(std::uint32_t id) { return Valid(id); }
	void aether_entity_destroy(std::uint32_t id)
	{
		AetherGameplayPropertyValue value{};
		Require(Invoke({.operation = AE_GAMEPLAY_SERVICE_DESTROY_ENTITY, .entity = Handle(id)}, value));
	}
	AetherGameplayVec3 aether_get_position(std::uint32_t id)
	{
		AetherGameplayVec3 result{};
		Require(s_engine.getPosition ? s_engine.getPosition(s_engine.context, Handle(id), &result) : AE_GAMEPLAY_UNSUPPORTED);
		return result;
	}
	void aether_set_position(std::uint32_t id, AetherGameplayVec3 position)
	{
		Require(s_engine.setPosition ? s_engine.setPosition(s_engine.context, Handle(id), position) : AE_GAMEPLAY_UNSUPPORTED);
	}
	int aether_input_key_down(int key)
	{
		AetherGameplayPropertyValue value{};
		Require(Invoke({.operation = AE_GAMEPLAY_SERVICE_KEY_DOWN, .integer = key}, value));
		return static_cast<int>(value.integer);
	}
	std::uint32_t aether_scene_find_by_name(const char* name)
	{
		AetherGameplayPropertyValue value{};
		return Invoke({.operation = AE_GAMEPLAY_SERVICE_FIND_ENTITY, .text = View(name)}, value) == AE_GAMEPLAY_OK ? value.entity.id : 0;
	}
	void aether_scene_load(const char* name)
	{
		AetherGameplayPropertyValue value{};
		Require(Invoke({.operation = AE_GAMEPLAY_SERVICE_LOAD_SCENE, .text = View(name)}, value));
	}
	std::uint32_t aether_scene_instantiate_prefab(const char* name, AetherGameplayVec3 position)
	{
		AetherGameplayPropertyValue value{};
		Require(Invoke({.operation = AE_GAMEPLAY_SERVICE_INSTANTIATE_PREFAB, .text = View(name)}, value));
		if (value.entity.id)
		{
			aether_set_position(value.entity.id, position);
		}
		return value.entity.id;
	}
	void aether_ui_set_text(std::uint32_t id, const char* text)
	{
		AetherGameplayPropertyValue value{};
		Require(Invoke({.operation = AE_GAMEPLAY_SERVICE_SET_UI_TEXT, .entity = Handle(id), .text = View(text)}, value));
	}
	struct Vec2 { float x, y; };
	void aether_physics2d_set_linear_velocity(std::uint32_t id, Vec2 velocity)
	{
		AetherGameplayPropertyValue value{};
		Require(Invoke({.operation = AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D, .entity = Handle(id), .vector = {velocity.x, velocity.y}}, value));
	}
	std::uint32_t aether_audio_play_2d(const char* path, float volume, float pitch, int loop, int bus)
	{
		if (volume != 1 || pitch != 1 || loop || bus != 1)
		{
			Error("WEB_AUDIO_UNSUPPORTED: this Web profile currently supports one-shot default-bus audio");
			return 0;
		}
		AetherGameplayPropertyValue value{};
		return Invoke({.operation = AE_GAMEPLAY_SERVICE_PLAY_AUDIO, .text = View(path)}, value) == AE_GAMEPLAY_OK ? 1 : 0;
	}
	int aether_save_read(const char* key, char* output, int capacity)
	{
		if (capacity < 0)
		{
			return 0;
		}
		std::uint32_t required = 0;
		AetherGameplayPropertyValue value{};
		const auto status = Invoke({.operation = AE_GAMEPLAY_SERVICE_READ_SAVE, .text = View(key)}, value, {output, static_cast<std::uint32_t>(capacity), &required});
		return status == AE_GAMEPLAY_OK || status == AE_GAMEPLAY_BUFFER_TOO_SMALL ? static_cast<int>(required) : -1;
	}
	int aether_save_write(const char* key, const char* value, int length)
	{
		if (length < 0 || (!value && length))
		{
			return 0;
		}
		AetherGameplayPropertyValue result{};
		return Invoke({.operation = AE_GAMEPLAY_SERVICE_WRITE_SAVE, .text = View(key), .text2 = {value, static_cast<std::uint32_t>(length)}}, result) == AE_GAMEPLAY_OK;
	}
	void aether_physics2d_enable_events(std::uint32_t id)
	{
		if (Valid(id) && !s_world->Has<aether::CollisionEvents2DComponent>(aether::Entity{id}))
		{
			s_world->Emplace<aether::CollisionEvents2DComponent>(aether::Entity{id});
		}
	}
	int aether_physics2d_event_count(std::uint32_t id, int kind)
	{
		if (!Valid(id) || kind < 0 || kind > 3)
		{
			return 0;
		}
		const auto* events = s_world->TryGet<aether::CollisionEvents2DComponent>(aether::Entity{id});
		if (!events)
		{
			return 0;
		}
		return static_cast<int>((kind == 0 ? events->collisionEnter : kind == 1 ? events->collisionExit : kind == 2 ? events->triggerEnter : events->triggerExit).size());
	}
	std::uint32_t aether_physics2d_event_at(std::uint32_t id, int kind, int index)
	{
		if (index < 0 || index >= aether_physics2d_event_count(id, kind))
		{
			return 0;
		}
		const auto& events = *s_world->TryGet<aether::CollisionEvents2DComponent>(aether::Entity{id});
		return (kind == 0 ? events.collisionEnter : kind == 1 ? events.collisionExit : kind == 2 ? events.triggerEnter : events.triggerExit)[index].id;
	}
}
