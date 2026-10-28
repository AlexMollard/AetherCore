#include "WebGameplay.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <emscripten.h>
#include <emscripten/html5.h>
#include "scripting/GameplayAbiValidation.hpp"
#include "scripting/Gameplay3D.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scripting/GameplayCustomPass.hpp"
#include "gpu/WebMaterialShader.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "io/FileSystem.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "ui/UiNavigationSystem.hpp"
#include "ui/UiWidgetSystem.hpp"
#include "scene/Hierarchy.hpp"
#include "scripting/GameplayRotation.hpp"
#if defined(AE_WEB_MANAGED)
#	include "BrowserManaged.hpp"
#endif

#if AE_WEB_TEST_BRIDGE && AE_WEB_PANIC_FIXTURE
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

		void ReportCustomPassFailure(const std::string& message)
		{
			EM_ASM(
			        {
				        aetherRuntime.status = 'failed';
				        aetherRuntime.diagnostics.push({code : 'WEB_CUSTOM_PASS_SHADER', message : UTF8ToString($0)});
			        },
			        message.c_str());
		}

		// Same relative-path rules as WebModels::ResolveShader, applied to a bare shader stem.
		bool ValidCustomPassStem(std::string_view stem)
		{
			if (stem.empty() || stem.size() > 128 || stem.find('\\') != std::string_view::npos || stem.find(':') != std::string_view::npos || stem.find('\0') != std::string_view::npos)
			{
				return false;
			}
			for (std::size_t start = 0; start <= stem.size();)
			{
				const auto end = stem.find('/', start);
				const auto segment = stem.substr(start, end == std::string_view::npos ? end : end - start);
				if (segment.empty() || segment == "." || segment == "..")
				{
					return false;
				}
				if (end == std::string_view::npos)
				{
					break;
				}
				start = end + 1;
			}
			return true;
		}
	} // namespace

#if AE_WEB_TEST_BRIDGE
	namespace
	{
		void AppendNumber(std::string& out, double value)
		{
			if (!std::isfinite(value))
			{
				out += "null";
				return;
			}
			char buffer[40];
			std::snprintf(buffer, sizeof(buffer), "%.17g", value);
			out += buffer;
		}

		void AppendString(std::string& out, const std::string& value)
		{
			out += '"';
			for (const unsigned char c: value)
			{
				if (c == '"' || c == '\\')
				{
					out += '\\';
					out += static_cast<char>(c);
				}
				else if (c < 0x20)
				{
					char buffer[8];
					std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
					out += buffer;
				}
				else
				{
					out += static_cast<char>(c);
				}
			}
			out += '"';
		}

		std::string EntityLabel(World& world, Entity entity)
		{
			const auto* name = world.TryGet<NameComponent>(entity);
			return name ? name->name : std::to_string(entity.id);
		}

		void AppendProperty(std::string& out, World& world, const ScriptPropertyValue& property)
		{
			switch (property.type)
			{
				case ScriptPropertyValue::Type::Float:
					AppendNumber(out, property.f4[0]);
					break;
				case ScriptPropertyValue::Type::Vector3:
					out += '[';
					for (int axis = 0; axis < 3; ++axis)
					{
						if (axis)
						{
							out += ',';
						}
						AppendNumber(out, property.f4[axis]);
					}
					out += ']';
					break;
				case ScriptPropertyValue::Type::Bool:
					out += property.i64 != 0 ? "true" : "false";
					break;
				case ScriptPropertyValue::Type::String:
					AppendString(out, property.str);
					break;
				case ScriptPropertyValue::Type::Entity:
				case ScriptPropertyValue::Type::Component:
				{
					const Entity target{static_cast<std::uint32_t>(property.i64)};
					if (world.GetRegistry().valid(World::ToEntt(target)) && world.TryGet<NameComponent>(target))
					{
						AppendString(out, world.TryGet<NameComponent>(target)->name);
					}
					else
					{
						out += "null";
					}
					break;
				}
				default:
					out += std::to_string(property.i64);
					break;
			}
		}
	} // namespace

	// Mirrors the native WriteGameplayState entity records (script properties and $engine
	// exists/position/localToWorld/uiText) so a harness can diff the two backends tick by tick.
	void WebGameplay::PublishTestState()
	{
		std::string out = "[";
		bool first = true;
		for (const auto& instance: m_instances)
		{
			if (!m_world.GetRegistry().valid(World::ToEntt(instance.entity)))
			{
				continue;
			}
			out += first ? "" : ",";
			first = false;
			out += "{\"entity\":";
			AppendString(out, EntityLabel(m_world, instance.entity));
			out += ",\"type\":";
			AppendString(out, instance.type);
			out += ",\"properties\":{";
			const auto properties = m_host.GetScriptProperties(instance.type);
			bool firstProperty = true;
			for (std::size_t index = 0; index < properties.size(); ++index)
			{
				ScriptPropertyValue value;
				if (!m_host.GetPropertyValue(instance.handle, static_cast<int>(index), value))
				{
					continue;
				}
				out += firstProperty ? "" : ",";
				firstProperty = false;
				AppendString(out, properties[index].name);
				out += ':';
				AppendProperty(out, m_world, value);
			}
			out += "}}";
		}
		for (auto&& [handle, name]: m_world.View<NameComponent>().each())
		{
			const Entity entity = World::FromEntt(handle);
			out += first ? "" : ",";
			first = false;
			out += "{\"entity\":";
			AppendString(out, name.name);
			out += ",\"type\":\"$engine\",\"properties\":{\"exists\":true";
			if (const auto* transform = m_world.TryGet<TransformComponent>(entity))
			{
				out += ",\"position\":[";
				for (int axis = 0; axis < 3; ++axis)
				{
					out += axis ? "," : "";
					AppendNumber(out, transform->localToWorld[3][axis]);
				}
				out += "],\"localToWorld\":[";
				for (int column = 0; column < 4; ++column)
				{
					for (int row = 0; row < 4; ++row)
					{
						out += (column || row) ? "," : "";
						AppendNumber(out, transform->localToWorld[column][row]);
					}
				}
				out += ']';
			}
			if (const auto* text = m_world.TryGet<ui::UIText>(entity))
			{
				out += ",\"uiText\":";
				AppendString(out, text->text);
			}
			out += "}}";
		}
		out += ']';
		EM_ASM({ aetherTestInternal.entityJson = UTF8ToString($0); aetherTestInternal.entityTick = $1; }, out.c_str(), m_ticks);
	}
#endif

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
#if defined(AE_WEB_MANAGED)
		BindManagedBrowserSimulation(nullptr, nullptr);
#endif
	}

	Expected<void> WebGameplay::Initialize()
	{
		m_scene = std::make_unique<WebScene>(m_world, m_graphics);
		m_scene->SetScriptTick(
		        [this](float dt)
		        {
			        // One failure ends the tick's script phase; Frame reports it once the world update returns.
			        if (m_script_status)
			        {
				        m_script_status = RunScripts(dt);
			        }
		        });
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

	Expected<void> WebGameplay::Load(const std::string& name, bool deferScripts)
	{
		if (!EM_ASM_INT({ return !globalThis.aetherProject || aetherProject.scenes.includes(UTF8ToString($0)); }, name.c_str()))
		{
			AE_UNEXPECTED(AetherError::Asset("WEB_DYNAMIC_SCENE_UNDECLARED: scene is not in the export manifest"));
		}
		m_host.Clear(++m_generation);
		m_instances.clear();
		m_spawned_scripts.clear();
		m_custom_passes.registered.clear();
		m_custom_passes.frame.passes.clear();
		AE_TRY(records, m_scene->Load(name));
#if defined(AE_WEB_MANAGED)
		// The shared C# interop exports act on this scene's physics and clip catalog.
		BindManagedBrowserSimulation(static_cast<PhysicsSystem*>(m_world.FindSystem("PhysicsSystem")), m_scene->Clips());
#endif
		m_manual_warm_frames = 0;
#if defined(AE_WEB_MANAGED)
		AE_TRY_VOID(m_host.Reset(m_module, "csharp", m_generation));
#else
		AE_TRY_VOID(m_host.Reset(m_module, "rust", m_generation));
#endif
		if (deferScripts)
		{
			// Native creates and attaches a freshly loaded scene's scripts on the NEXT
			// ScriptComponentSystem update, never inside the update that requested the load.
			m_spawned_scripts = std::move(*records);
		}
		else
		{
			for (const auto& record: *records)
			{
				AE_TRY(handle, m_host.CreateInstance(record.record.type, record.entity.id));
				EnablePhysicsEvents(record.entity);
				m_instances.push_back({record.entity, *handle, record.record.type});
				AE_TRY_VOID(m_host.TryApplyProperties(*handle, record.record.type, record.record.properties));
			}
			for (const auto& instance: m_instances)
			{
				AE_TRY_VOID(m_host.Attach(instance.handle));
			}
		}
		EM_ASM({ aetherRuntime.scene = UTF8ToString($0); }, name.c_str());
		m_previous = m_accumulator = 0;
		return {};
	}

	// Native ScriptComponentSystem gives a Rust instance's entity the 3D contact lists when a
	// PhysicsSystem exists: Jolt fills them only for entities that carry the component.
	void WebGameplay::EnablePhysicsEvents(Entity entity)
	{
#if !defined(AE_WEB_MANAGED)
		if ((m_module.capabilities & AE_GAMEPLAY_CAP_PHYSICS) != 0 && m_world.FindSystem("PhysicsSystem") != nullptr && !m_world.Has<CollisionEventsComponent>(entity))
		{
			m_world.Emplace<CollisionEventsComponent>(entity);
		}
#else
		(void) entity;
#endif
	}

	// ScriptComponentSystem::Update: every attached script, in creation order, gets the physics
	// events produced by this tick's step and then its Update. Instances created while this runs
	// (scripted prefab instantiation) start with the next tick, like a native script added mid-update.
	Expected<void> WebGameplay::RunScripts(float dt)
	{
		AE_TRY_VOID(StartSpawnedScripts());
		const auto count = m_instances.size();
		for (std::size_t index = 0; index < count && index < m_instances.size(); ++index)
		{
			const auto instance = m_instances[index];
			if (!m_world.GetRegistry().valid(World::ToEntt(instance.entity)) || m_world.Has<DisabledComponent>(instance.entity) || ecs::HasDisabledAncestor(m_world, instance.entity))
			{
				continue;
			}
			if ((m_module.capabilities & AE_GAMEPLAY_CAP_PHYSICS) != 0)
			{
				// Snapshot first: callbacks may add components and invalidate ECS storage.
				const auto dispatch = [&](const auto& snapshot, std::uint32_t flags) -> Expected<void>
				{
					const std::vector<Entity>* lists[] = {&snapshot.collisionEnter, &snapshot.collisionExit, &snapshot.triggerEnter, &snapshot.triggerExit, &snapshot.overlapping};
					for (std::uint32_t kind = 0; kind < 5; ++kind)
					{
						for (const auto other: *lists[kind])
						{
							AE_TRY_VOID(m_host.DispatchPhysics(instance.handle, other.id, kind, flags));
						}
					}
					return {};
				};
				if (const auto* events = m_world.TryGet<CollisionEvents2DComponent>(instance.entity))
				{
					AE_TRY_VOID(dispatch(CollisionEvents2DComponent(*events), 0));
				}
				if (const auto* events = m_world.TryGet<CollisionEventsComponent>(instance.entity))
				{
					AE_TRY_VOID(dispatch(CollisionEventsComponent(*events), AE_GAMEPLAY_PHYSICS_FLAG_3D));
				}
			}
			AE_TRY_VOID(m_host.Update(instance.handle, dt));
		}
		AE_TRY_VOID(m_scene->ValidateEffects());
		// Entity.Destroy from script is deferred to here, ahead of the particle system, so no
		// callback frees storage the loop above iterates (native: end of ScriptComponentSystem::Update).
		AE_TRY_VOID(ProcessPendingDestroys());
		// Native applies a script-requested scene load here, once the frame's scripts have finished
		// (ScriptComponentSystem::Update), so the new scene's particles and the checkpoint of this
		// very tick already see it. Frame timing is preserved across the switch.
		if (!m_pending_scene.empty())
		{
			const auto scene = std::exchange(m_pending_scene, {});
			const auto previous = m_previous;
			const auto accumulator = m_accumulator;
			AE_TRY_VOID(Load(scene, true));
			m_previous = previous;
			m_accumulator = accumulator;
		}
		return {};
	}

	// Scripts on entities spawned during the previous tick: create, configure and attach, as the
	// native ScriptComponentSystem does the first time it sees them.
	Expected<void> WebGameplay::StartSpawnedScripts()
	{
		const auto pending = std::exchange(m_spawned_scripts, {});
		const auto first = m_instances.size();
		for (const auto& record: pending)
		{
			if (!m_world.GetRegistry().valid(World::ToEntt(record.entity)))
			{
				continue;
			}
			AE_TRY(handle, m_host.CreateInstance(record.record.type, record.entity.id));
			EnablePhysicsEvents(record.entity);
			m_instances.push_back({record.entity, *handle, record.record.type});
			AE_TRY_VOID(m_host.TryApplyProperties(*handle, record.record.type, record.record.properties));
		}
		for (auto index = first; index < m_instances.size(); ++index)
		{
			AE_TRY_VOID(m_host.Attach(m_instances[index].handle));
		}
		return {};
	}

	Expected<void> WebGameplay::ProcessPendingDestroys()
	{
		const auto pending = std::exchange(m_pending_destroys, {});
		for (const auto entity: pending)
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
		return {};
	}

	Expected<void> WebGameplay::Frame(double now, bool& capture)
	{
		if (EM_ASM_INT({ return aetherPlatform.failed; }))
		{
			AE_UNEXPECTED(AetherError::Engine("Browser platform service failed"));
		}
#if AE_WEB_TEST_BRIDGE
		const bool testManual = EM_ASM_INT({ return aetherTestInternal.manual(); });
		const bool testStep = !m_manual_pending_frame && EM_ASM_INT({ return aetherTestInternal.beginFrame(); });
		const auto ticksBeforeFrame = m_ticks;
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
		m_mouse_delta += glm::vec2{static_cast<float>(EM_ASM_DOUBLE({ const v = aetherInput.dx; aetherInput.dx = 0; return v; })),
		        static_cast<float>(EM_ASM_DOUBLE({ const v = aetherInput.dy; aetherInput.dy = 0; return v; }))};
		m_scroll_delta.y += static_cast<float>(EM_ASM_DOUBLE({ const v = aetherInput.scroll; aetherInput.scroll = 0; return v; }));
		if (!canvasFocused)
		{
			m_mouse_delta = m_scroll_delta = {};
		}
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
#if AE_WEB_PANIC_FIXTURE
					aether_browser_test_panic();
#else
					AE_UNEXPECTED(AetherError::Engine("Browser automation panic fixture"));
#endif
				}
#endif
				// Submissions always reflect the last tick: re-submitted every tick, never accumulated.
				m_custom_passes.frame.passes.clear();
				// Native order (Application.cpp): Animation, SpriteAnimation, Physics, Physics2D,
				// ScriptComponentSystem, Particle. WebScene registers WebScriptSystem at the script
				// slot, so every script receives this tick's physics events and its Update after the
				// physics step and before particles - a velocity a script sets is first integrated
				// on the following tick, and a burst it queues emits the same tick, as on desktop.
				m_world.UpdateSystems(1.0f / 60.0f);
				m_mouse_delta = m_scroll_delta = {};
				AE_TRY_VOID(std::exchange(m_script_status, {}));
				for (const auto entity: m_scene->TakeRetiredParticles())
				{
					m_pending_destroys.push_back(entity);
				}
				m_scene->Tick(1.0f / 60.0f);
				AE_TRY_VOID(ProcessPendingDestroys());
				// A render frame may not run a fixed tick. Retain short presses until gameplay consumes them.
				m_pressed_keys.fill(false);
				++m_ticks;
#if AE_WEB_TEST_BRIDGE
				if (testStep)
				{
					EM_ASM({ aetherTestInternal.afterTick(); });
				}
				if (testStep)
				{
					PublishTestState();
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
#if AE_WEB_TEST_BRIDGE
		const bool advanceTemporal = testManual && (m_ticks != ticksBeforeFrame || m_manual_pending_advance);
		if (testManual && !testStep && !m_manual_pending_frame && !capture && m_manual_warm_frames >= 3
		        && width == m_last_render_width && height == m_last_render_height && !m_scene->StreamingPending())
		{
			return {};
		}
		// Cache every bounded manual-step readback so capture() observes the actual
		// moving tick rather than submitting a second, motionless temporal frame.
		if (testManual && advanceTemporal) { capture = true; }
#endif
		RenderFramePacket packet;
		packet.frameIndex = m_frames;
		packet.simulationTick = m_ticks;
#if AE_WEB_TEST_BRIDGE
		if (testManual)
		{
			packet.advanceTemporalEffects = advanceTemporal;
			packet.temporalFrameIndex = m_ticks > 0 ? m_ticks - 1 : 0;
		}
#endif
		AE_TRY_VOID(m_scene->Extract(packet, width, height, m_ticks / 60.0f));
		packet.renderCustom = m_custom_passes.frame;
		const bool readbackRequested = capture;
		const bool accepted = m_scene->RenderReady() && m_graphics.Draw(packet, capture);
		if (accepted)
		{
			++m_frames;
			capture = false;
			m_last_render_width = width;
			m_last_render_height = height;
			EM_ASM({ aetherRuntime.lastRenderedFrameIndex = $0; aetherRuntime.renderSceneGeneration = $1; aetherRuntime.lastRenderedFrameCaptured = !!$2; }, static_cast<double>(packet.frameIndex), static_cast<double>(packet.renderSceneGeneration), readbackRequested);
		}
#if AE_WEB_TEST_BRIDGE
		if (testManual)
		{
			m_manual_pending_frame = !accepted;
			m_manual_pending_advance = !accepted && advanceTemporal;
			if (accepted)
			{
				m_manual_warm_frames = advanceTemporal ? 3 : m_manual_warm_frames + 1;
			}
		}
#endif
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
		if (accepted) { EM_ASM({ aetherTestInternal.endFrame(); }); }
#endif
		return {};
	}

	AetherGameplayStatus WebGameplay::Invoke(const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& value, AetherGameplayUtf8Buffer buffer)
	{
		// SUBMIT_CUSTOM_PASS and RAYCAST_3D carry raw float32 bytes in text2, not text.
		if (!aether::scripting::IsGameplayUtf8(request.text) || (!aether::scripting::gameplay3d::HasRawText2(request.operation) && !aether::scripting::IsGameplayUtf8(request.text2)))
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
		const bool entityOperation = request.operation == 2 || request.operation == 3 || request.operation == 4 || request.operation == 9 || request.operation == 13 || request.operation == AE_GAMEPLAY_SERVICE_SET_EULER || aether::scripting::gameplay3d::IsGameplay3DEntityOperation(request.operation);
		if (entityOperation)
		{
			std::uint32_t valid = 0;
			const auto status = engine.entityValid(engine.context, request.entity, &valid);
			if (status != AE_GAMEPLAY_OK)
			{
				return status;
			}
		}
		if (aether::scripting::IsCustomPassOperation(request.operation))
		{
			if (request.operation == AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS && request.text2.data && request.text2.length)
			{
				const std::string stem(request.text2.data, request.text2.length);
				if (!m_custom_shaders.contains(stem))
				{
					if (!ValidCustomPassStem(stem))
					{
						ReportCustomPassFailure("WEB_CUSTOM_PASS_SHADER: invalid shader stem: " + stem);
						return AE_GAMEPLAY_INVALID_ARGUMENT;
					}
					const std::string cooked = "project://assets/shaders/" + stem + ".wshader";
					const auto bytes = io::FileSystem::ReadFile(cooked);
					if (!bytes)
					{
						ReportCustomPassFailure("WEB_CUSTOM_PASS_SHADER: cannot read " + cooked + ": " + bytes.error().message);
						return AE_GAMEPLAY_INVALID_ARGUMENT;
					}
					const auto source = gpu::DecodeWebMaterialShader(*bytes);
					if (!source)
					{
						ReportCustomPassFailure("WEB_CUSTOM_PASS_SHADER: " + source.error() + ": " + cooked);
						return AE_GAMEPLAY_INVALID_ARGUMENT;
					}
					if (!m_graphics.UploadCustomPassShader(stem, *source))
					{
						ReportCustomPassFailure("WEB_CUSTOM_PASS_SHADER: " + stem + ": " + m_graphics.CustomPassError());
						return AE_GAMEPLAY_INVALID_ARGUMENT;
					}
					m_custom_shaders.insert(stem);
				}
			}
			return aether::scripting::ExecuteCustomPassOperation(m_custom_passes, request);
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
			case AE_GAMEPLAY_SERVICE_MOUSE_DOWN:
				if (request.integer < 0 || request.integer > 2)
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = m_input.IsMouseButtonDown(static_cast<MouseButton>(request.integer));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_MOUSE_DELTA:
			case AE_GAMEPLAY_SERVICE_SCROLL_DELTA:
			{
				const auto delta = request.operation == AE_GAMEPLAY_SERVICE_MOUSE_DELTA ? m_mouse_delta : m_scroll_delta;
				value.type = AE_GAMEPLAY_PROPERTY_VECTOR3;
				value.vector[0] = delta.x;
				value.vector[1] = delta.y;
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_SET_EULER:
			{
				const auto status = aether::scripting::SetGameplayEuler(m_world, entity, request.vector, static_cast<PhysicsSystem*>(m_world.FindSystem("PhysicsSystem")));
				if (status != AE_GAMEPLAY_OK)
				{
					return status;
				}
				if (auto* physics = static_cast<Physics2DSystem*>(m_world.FindSystem("Physics2DSystem")))
				{
					physics->TeleportToTransform(m_world, entity);
				}
				return AE_GAMEPLAY_OK;
			}
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
				auto created = m_scene->Instantiate(text, {request.vector[0], request.vector[1], request.vector[2]});
				if (!created)
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				m_spawned_scripts.insert(m_spawned_scripts.end(), std::make_move_iterator(created->scripts.begin()), std::make_move_iterator(created->scripts.end()));
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.entity = m_access.Handle(created->root);
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
			{
				if (!aether::scripting::gameplay3d::IsGameplay3DOperation(request.operation))
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				// Same resolution as the native host: Jolt where a PhysicsSystem exists, none otherwise.
				auto* physics = static_cast<PhysicsSystem*>(m_world.FindSystem("PhysicsSystem"));
				if (physics == nullptr && request.operation < AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP)
				{
					return AE_GAMEPLAY_UNSUPPORTED;
				}
				const auto result = aether::scripting::gameplay3d::ExecuteGameplay3DOperation(m_world, physics, request, value, buffer, m_scene ? m_scene->Clips() : nullptr);
				if (result.status == AE_GAMEPLAY_OK && result.hasEntity)
				{
					value.entity = m_access.Handle(result.entity);
				}
				return result.status;
			}
		}
	}
} // namespace aether::app::web
