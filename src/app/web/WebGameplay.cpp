#include "WebGameplay.hpp"

#include <cmath>
#include <cstring>
#include <emscripten.h>
#include <emscripten/html5.h>
#include "scripting/GameplayAbiValidation.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "io/FileSystem.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "ui/UiNavigationSystem.hpp"
#include "ui/UiWidgetSystem.hpp"
#include "scene/Hierarchy.hpp"

#if AE_WEB_TEST_BRIDGE
extern "C" void aether_browser_test_panic();
#endif

namespace aether::app::web
{
	namespace
	{
		std::string Text(AetherGameplayUtf8 value)
		{
			return value.length ? std::string(value.data, value.length) : std::string{};
		}
	} // namespace

	WebGameplay::WebGameplay(World& world, aether::scripting::GameplayAccess& access, webgpu::WebGpuDevice& graphics, std::array<bool, 512>& keys, std::array<bool, 512>& pressedKeys, const AetherGameplayModuleApi& module)
	      : m_world(world), m_access(access), m_graphics(graphics), m_keys(keys), m_pressed_keys(pressedKeys), m_module(module)
	{
		m_host.SetAccess(&access);
		m_host.SetErrorHandler(
		        [](const std::string& message)
		        {
			        EM_ASM(
			                {
				                aetherRuntime.status = 'failed';
				                aetherRuntime.diagnostics.push({code : 'BROWSER_SCRIPT', message : UTF8ToString($0)});
			                },
			                message.c_str());
		        });
	}

	WebGameplay::~WebGameplay()
	{
		Stop();
	}

	void WebGameplay::Stop()
	{
		m_host.Clear(++m_generation);
		m_instances.clear();
		m_scene.reset();
	}

	Expected<void> WebGameplay::Initialize()
	{
		m_scene = std::make_unique<WebScene>(m_world, m_graphics);
		const int length =
		        EM_ASM_INT({ return lengthBytesUTF8(new URLSearchParams(location.search).get('scene') || (globalThis.aetherProject ?.startupScene || (new URLSearchParams(location.search).has('resume') ? 'ReferenceResult' : 'ReferenceStart'))); });
		if (length < 1 || length > 128)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid browser startup scene name"));
		}
		std::string name(length, '\0');
		EM_ASM(
		        { stringToUTF8(new URLSearchParams(location.search).get('scene') || (globalThis.aetherProject ?.startupScene || (new URLSearchParams(location.search).has('resume') ? 'ReferenceResult' : 'ReferenceStart')), $0, $1); },
		        name.data(),
		        name.size() + 1);
		return Load(name);
	}

	Expected<void> WebGameplay::Load(const std::string& name)
	{
		if (!EM_ASM_INT({ return !globalThis.aetherProject || aetherProject.scenes.includes(UTF8ToString($0)); }, name.c_str()))
		{
			AE_UNEXPECTED(AetherError::Asset("WEB_DYNAMIC_SCENE_UNDECLARED: scene is not in the export manifest"));
		}
		m_host.Clear(++m_generation);
		m_instances.clear();
		AE_TRY(records, m_scene->Load(name));
#if defined(AE_WEB_MANAGED)
		AE_TRY_VOID(m_host.Reset(m_module, "csharp", m_generation));
#else
		AE_TRY_VOID(m_host.Reset(m_module, "rust", m_generation));
#endif
		for (const auto& record: *records)
		{
			AE_TRY(handle, m_host.CreateInstance(record.record.type, record.entity.id));
			m_instances.push_back({record.entity, *handle});
			AE_TRY_VOID(m_host.TryApplyProperties(*handle, record.record.type, record.record.properties));
		}
		for (const auto& instance: m_instances)
		{
			AE_TRY_VOID(m_host.Attach(instance.handle));
		}
		EM_ASM({ aetherRuntime.scene = UTF8ToString($0); }, name.c_str());
		m_previous = m_accumulator = 0;
		return {};
	}

	Expected<void> WebGameplay::Frame(double now, bool& capture)
	{
		if (EM_ASM_INT({ return aetherPlatform.failed; }))
		{
			AE_UNEXPECTED(AetherError::Engine("Browser platform service failed"));
		}
#if AE_WEB_TEST_BRIDGE
		const bool testStep = EM_ASM_INT({ return aetherTestInternal.beginFrame(); });
		std::array<std::uint8_t, 512> testKeys{};
		EM_ASM({ HEAPU8.set(aetherTestInternal.keys, $0); }, testKeys.data());
#endif
		const bool canvasFocused = EM_ASM_INT({ return document.activeElement == Module.canvas && document.hasFocus() && !document.hidden; });
		for (size_t key = 0; key < m_keys.size(); ++key)
		{
			m_input.SetSyntheticKey(static_cast<int>(key),
			        canvasFocused
			                && (m_keys[key] || m_pressed_keys[key]
#if AE_WEB_TEST_BRIDGE
			                        || testKeys[key]
#endif
			                        ),
			        false);
		}
		const int buttons = EM_ASM_INT({
			const buttons = aetherInput.buttons | aetherInput.pressed;
			aetherInput.pressed = 0;
			return buttons;
		});
		for (int button = 0; button < 3; ++button)
		{
			m_input.SetSyntheticMouseButton(button, canvasFocused && (buttons & (1 << button)));
		}
		m_input.SetSyntheticMousePos({EM_ASM_DOUBLE({ return aetherInput.x; }), EM_ASM_DOUBLE({ return aetherInput.y; })});
		m_input.Update();
		EM_ASM(
		        {
			        aetherRuntime.gamepadConnected = !!$0;
			        aetherRuntime.padA = !!$1;
			        aetherRuntime.padLeftX = $2;
		        },
		        m_input.IsGamepadConnected(),
		        m_input.IsGamepadButtonDown(GamepadButton::A),
		        m_input.GetGamepadStick(GamepadStick::Left).x);
		int uiWidth = 0, uiHeight = 0;
		emscripten_get_canvas_element_size("#canvas", &uiWidth, &uiHeight);
		ui::ResolveCanvases(m_world, {uiWidth, uiHeight});
		ui::UiNavigationSystem::Update(m_world, m_input);
		ui::UiWidgetSystem::Update(m_world, m_input, static_cast<float>(now / 1000.0));
		const bool paused = EM_ASM_INT({
			return document.hidden || !document.hasFocus() || !aetherPlatform.storageReady || !aetherPlatform.started || (aetherPlatform.audioSources.size > 0 && !aetherPlatform.audioReady) || aetherPlatform.busy || aetherPlatform.pending.size > 0;
		});
		if (paused)
		{
			m_previous = m_accumulator = 0;
			EM_ASM({
				if (aetherPlatform.pending.size && !aetherPlatform.busy)
				{
					aetherPlatform.flush();
				}
			});
		}
		else
		{
			if (!m_pending_scene.empty())
			{
				const auto scene = std::exchange(m_pending_scene, {});
				AE_TRY_VOID(Load(scene));
			}
			const double delta = m_previous == 0 ? 0 : std::clamp((now - m_previous) / 1000.0, 0.0, 0.1);
			m_previous = now;
			m_accumulator += delta;
#if AE_WEB_TEST_BRIDGE
			if (EM_ASM_INT({ return aetherTestInternal.manual(); }))
			{
				m_accumulator = testStep ? 1.0 / 60.0 : 0;
			}
#endif
			while (m_accumulator >= 1.0 / 60.0)
			{
#if AE_WEB_TEST_BRIDGE
				if (EM_ASM_INT({ return aetherTestInternal.takeFault(); }))
				{
					aether_browser_test_panic();
				}
#endif
				for (const auto& instance: m_instances)
				{
					if (m_world.Has<DisabledComponent>(instance.entity) || ecs::HasDisabledAncestor(m_world, instance.entity))
					{
						continue;
					}
					AE_TRY_VOID(m_host.Update(instance.handle, 1.0f / 60.0f));
				}
				m_world.UpdateSystems(1.0f / 60.0f);
				m_scene->Tick(1.0f / 60.0f);
				for (const auto& instance: m_instances)
				{
					if ((m_module.capabilities & AE_GAMEPLAY_CAP_PHYSICS) == 0)
					{
						continue;
					}
					if (const auto* events = m_world.TryGet<CollisionEvents2DComponent>(instance.entity))
					{
						for (auto other: events->collisionEnter)
						{
							AE_TRY_VOID(m_host.DispatchPhysics(instance.handle, other.id, 0));
						}
						for (auto other: events->collisionExit)
						{
							AE_TRY_VOID(m_host.DispatchPhysics(instance.handle, other.id, 1));
						}
						for (auto other: events->triggerEnter)
						{
							AE_TRY_VOID(m_host.DispatchPhysics(instance.handle, other.id, 2));
						}
						for (auto other: events->triggerExit)
						{
							AE_TRY_VOID(m_host.DispatchPhysics(instance.handle, other.id, 3));
						}
					}
				}
				for (auto entity: m_pending_destroys)
				{
					for (auto instance = m_instances.begin(); instance != m_instances.end();)
					{
						if (ecs::IsAncestor(m_world, instance->entity, entity))
						{
							AE_TRY_VOID(m_host.Detach(instance->handle));
							AE_TRY_VOID(m_host.Destroy(instance->handle));
							instance = m_instances.erase(instance);
						}
						else
						{
							++instance;
						}
					}
					m_world.Destroy(entity);
				}
				m_pending_destroys.clear();
				// A render frame may not run a fixed tick. Retain short presses until gameplay consumes them.
				m_pressed_keys.fill(false);
				++m_ticks;
#if AE_WEB_TEST_BRIDGE
				if (testStep)
				{
					EM_ASM({ aetherTestInternal.afterTick(); });
				}
#endif
				m_accumulator -= 1.0 / 60.0;
				if (!m_pending_scene.empty() || EM_ASM_INT({ return aetherPlatform.pending.size > 0; }))
				{
					break;
				}
			}
		}
		unsigned width = 0, height = 0;
		emscripten_get_canvas_element_size("#canvas", reinterpret_cast<int*>(&width), reinterpret_cast<int*>(&height));
		RenderFramePacket packet;
		packet.frameIndex = m_frames;
		AE_TRY_VOID(m_scene->Extract(packet, width, height, m_ticks / 60.0f));
		if (m_graphics.Draw(packet, capture))
		{
			++m_frames;
			capture = false;
		}
		for (const auto entity: m_world.View<ui::UIText>())
		{
			const auto& text = m_world.GetRegistry().get<ui::UIText>(entity).text;
			EM_ASM({ aetherRuntime.uiText = UTF8ToString($0); }, text.c_str());
		}
		for (const auto raw: m_world.View<ui::UISelectable, NameComponent>())
		{
			if (m_world.GetRegistry().get<ui::UISelectable>(raw).focused)
			{
				EM_ASM({ aetherRuntime.focusedName = UTF8ToString($0); }, m_world.GetRegistry().get<NameComponent>(raw).name.c_str());
			}
		}
		for (const auto raw: m_world.View<ui::UIToggle>())
		{
			EM_ASM({ aetherRuntime.toggleOn = !!$0; }, m_world.GetRegistry().get<ui::UIToggle>(raw).on);
		}
		for (const auto raw: m_world.View<ui::UISlider>())
		{
			EM_ASM({ aetherRuntime.sliderValue = $0; }, m_world.GetRegistry().get<ui::UISlider>(raw).value);
		}
		for (const auto raw: m_world.View<SpriteAnimatorComponent>())
		{
			EM_ASM({ aetherRuntime.animationFrame = $0; }, m_world.GetRegistry().get<SpriteAnimatorComponent>(raw).currentFrame);
		}
		EM_ASM(
		        {
			        aetherRuntime.ticks = $0;
			        aetherRuntime.frames = $1;
			        aetherRuntime.rendererReady = true;
		        },
		        m_ticks,
		        m_frames);
#if AE_WEB_TEST_BRIDGE
		EM_ASM({ aetherTestInternal.endFrame(); });
#endif
		return {};
	}

	AetherGameplayStatus WebGameplay::Invoke(const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& value, AetherGameplayUtf8Buffer buffer)
	{
		if (!aether::scripting::IsGameplayUtf8(request.text) || !aether::scripting::IsGameplayUtf8(request.text2))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		for (float number: request.vector)
		{
			if (!std::isfinite(number))
			{
				return AE_GAMEPLAY_INVALID_ARGUMENT;
			}
		}
		value = {};
		value.structSize = sizeof(value);
		const auto engine = m_access.EngineApi();
		const Entity entity{request.entity.id};
		const bool entityOperation = request.operation == 2 || request.operation == 3 || request.operation == 4 || request.operation == 9 || request.operation == 13;
		if (entityOperation)
		{
			std::uint32_t valid = 0;
			const auto status = engine.entityValid(engine.context, request.entity, &valid);
			if (status != AE_GAMEPLAY_OK)
			{
				return status;
			}
		}
		const auto text = Text(request.text);
		switch (request.operation)
		{
			case AE_GAMEPLAY_SERVICE_KEY_DOWN:
				if (request.integer < 0 || request.integer >= m_keys.size())
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = m_input.IsKeyDown(static_cast<Key>(request.integer));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_SET_SPRITE_COLOR:
				if (auto* sprite = m_world.TryGet<SpriteRendererComponent>(entity))
				{
					sprite->tint = {request.vector[0], request.vector[1], request.vector[2], request.vector[3]};
					return AE_GAMEPLAY_OK;
				}
				return AE_GAMEPLAY_UNSUPPORTED;
			case AE_GAMEPLAY_SERVICE_GET_VELOCITY_2D:
			case AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D:
			{
				auto* body = m_world.TryGet<RigidBody2DComponent>(entity);
				auto* physics = static_cast<Physics2DSystem*>(m_world.FindSystem("Physics2DSystem"));
				if (!body || !body->body.IsValid() || !physics)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				if (request.operation == AE_GAMEPLAY_SERVICE_SET_VELOCITY_2D)
				{
					physics->SetLinearVelocity(body->body, {request.vector[0], request.vector[1]});
				}
				else
				{
					const auto velocity = physics->GetLinearVelocity(body->body);
					value.type = AE_GAMEPLAY_PROPERTY_VECTOR3;
					value.vector[0] = velocity.x;
					value.vector[1] = velocity.y;
				}
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_FIND_ENTITY:
				for (const auto raw: m_world.View<NameComponent>())
				{
					if (m_world.GetRegistry().get<NameComponent>(raw).name == text)
					{
						value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
						value.entity = m_access.Handle(World::FromEntt(raw));
						return AE_GAMEPLAY_OK;
					}
				}
				return AE_GAMEPLAY_INVALID_HANDLE;
			case AE_GAMEPLAY_SERVICE_LOAD_SCENE:
				if (!io::FileSystem::Exists(WebScene::ProjectPath("scenes", text, ".scene.toml")))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				m_pending_scene = text;
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_INSTANTIATE_PREFAB:
			{
				if (!m_scene)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				auto created = m_scene->Instantiate(text);
				if (!created)
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.entity = m_access.Handle(*created);
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_PLAY_AUDIO:
				return EM_ASM_INT({ return aetherPlatform.play(UTF8ToString($0, $1)); }, text.data(), text.size()) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_UNSUPPORTED;
			case AE_GAMEPLAY_SERVICE_SET_UI_TEXT:
				if (auto* label = m_world.TryGet<ui::UIText>(entity))
				{
					label->text = text;
					return AE_GAMEPLAY_OK;
				}
				return AE_GAMEPLAY_UNSUPPORTED;
			case AE_GAMEPLAY_SERVICE_WRITE_SAVE:
				return EM_ASM_INT(
				               {
					               const decoder = new TextDecoder();
					               return aetherPlatform.stage(decoder.decode(HEAPU8.subarray($0, $0 + $1)), decoder.decode(HEAPU8.subarray($2, $2 + $3)));
				               },
				               request.text.data,
				               request.text.length,
				               request.text2.data,
				               request.text2.length)
				               ? AE_GAMEPLAY_OK
				               : AE_GAMEPLAY_INVALID_ARGUMENT;
			case AE_GAMEPLAY_SERVICE_READ_SAVE:
			{
				if (!buffer.required || (!buffer.data && buffer.capacity))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				const int length = EM_ASM_INT(
				        {
					        const value = aetherPlatform.read(UTF8ToString($0, $1));
					        if (typeof value != 'string')
					        {
						        return -1;
					        }
					        const bytes = new TextEncoder().encode(value);
					        if (bytes.length > 65536)
					        {
						        return -1;
					        }
					        if ($3 >= bytes.length + 1)
					        {
						        HEAPU8.set(bytes, $2);
						        HEAPU8[$2 + bytes.length] = 0;
					        }
					        return bytes.length;
				        },
				        text.data(),
				        text.size(),
				        buffer.data,
				        buffer.capacity);
				if (length < 0)
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				*buffer.required = length + 1;
				value.type = AE_GAMEPLAY_PROPERTY_STRING;
				return buffer.capacity < length + 1 ? AE_GAMEPLAY_BUFFER_TOO_SMALL : AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_CREATE_ENTITY:
			{
				const auto created = m_world.Create();
				m_world.Emplace<NameComponent>(created, NameComponent{.name = text});
				m_world.Emplace<TransformComponent>(created).localToWorld[3] = {request.vector[0], request.vector[1], request.vector[2], 1};
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.entity = m_access.Handle(created);
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_DESTROY_ENTITY:
				m_pending_destroys.push_back(entity);
				return AE_GAMEPLAY_OK;
			default:
				return AE_GAMEPLAY_UNSUPPORTED;
		}
	}
} // namespace aether::app::web
