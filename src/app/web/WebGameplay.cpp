#include "WebGameplay.hpp"
#include "WebStreamingSnapshot.hpp"
#include "WebStreamingBackend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <emscripten.h>
#include <emscripten/html5.h>
#include "audio/AudioSubsystem.hpp"
#include "audio/AudioSystem.hpp"
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
#include "ui/UiTextBoxSystem.hpp"
#include "ui/UiWidgetSystem.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/SceneStreamingReferences.hpp"
#include "scripting/GameplayRotation.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "net/NetSpawn.hpp"
#include "net/GameplayNetworkServices.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/ServiceContainer.hpp"
#if defined(AE_WEB_MANAGED)
#	include "BrowserManaged.hpp"
#	include "scripting/SceneContext.hpp"
#	include "net/BrowserScriptBridges.hpp"
#endif

#if AE_WEB_TEST_BRIDGE && AE_WEB_PANIC_FIXTURE
extern "C" void aether_browser_test_panic();
#endif

namespace aether::app::web
{
	namespace
	{
		struct ActorScope
		{
			Entity& active;
			Entity previous;
			ActorScope(Entity& target, Entity actor) : active(target), previous(std::exchange(target, actor)) {}
			~ActorScope() { active = previous; }
		};

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
	std::uint64_t WebGameplay::GetInstanceHandle(Entity entity, std::uint32_t scriptIndex) const
	{
		for (const auto& instance : m_instances)
		{
			if (instance.entity == entity && instance.scriptIndex == scriptIndex)
				return instance.handle;
		}
		return 0;
	}


	void WebGameplay::Stop()
	{
		if (m_streaming)
		{
#if defined(AE_WEB_MANAGED)
			ManagedBrowserSceneContext().streaming = nullptr;
#endif
			m_streaming->Shutdown();
			m_streaming.reset();
			m_starting_regions.clear();
			m_script_restores.clear();
		}
		m_world.UnregisterSystem("AudioSystem");
#if defined(AE_WEB_MANAGED)
		ManagedBrowserSceneContext().audio = nullptr;
		ManagedBrowserSceneContext().input = nullptr;
#endif
		m_audio.reset();
		if (m_network)
			m_network->Stop(m_world);
		m_world.UnregisterSystem("NetworkSendSystem");
		m_world.UnregisterSystem("NetworkReceiveSystem");
		m_host.Clear(++m_generation);
		m_host_ready = false;
		m_streaming_budget.reset();
		m_instances.clear();
		if (auto* pending = m_world.GetRegistry().ctx().find<scene::detail::StreamingScriptNodeReferences>())
		{
			pending->entries.clear();
		}
		m_scene.reset();
#if defined(AE_WEB_MANAGED)
		BindManagedBrowserSimulation(nullptr, nullptr);
		BindManagedBrowserServices(nullptr);
#endif
		m_network.reset();
		m_services.reset();
	}

	Expected<void> WebGameplay::Initialize()
	{
		m_services = std::make_unique<ServiceContainer>();
		m_audio = std::make_unique<audio::AudioSubsystem>();
		AE_TRY_VOID(m_audio->Init());
		m_services->Register(*m_audio);
#if defined(AE_WEB_MANAGED)
		ManagedBrowserSceneContext().audio = m_audio.get();
		ManagedBrowserSceneContext().input = &m_input;
#endif
		m_network = std::make_unique<net::NetworkContext>(*m_services);
		m_services->Register(*m_network);
		auto readNetworkSetting = [](const char* key)
		{
			const int length = EM_ASM_INT({ return lengthBytesUTF8(globalThis.aetherProject?.network?.[UTF8ToString($0)] || ''); }, key);
			std::string value(length, '\0');
			EM_ASM({ stringToUTF8(globalThis.aetherProject?.network?.[UTF8ToString($0)] || '', $1, $2); }, key, value.data(), value.size() + 1);
			return value;
		};
		const auto gateway = readNetworkSetting("gatewayUrl");
		const auto token = readNetworkSetting("authToken");
		const auto target = readNetworkSetting("targetAlias");
		m_network->Transport().ConfigureWebGateway(gateway, token, {}, target);
		m_network->SetWebPrefabSpawner([this](std::string_view name, glm::vec3 position)
		{
			auto created = m_scene->Instantiate(name, position);
			if (!created) return Entity{};
			const Entity root = created->root;
			AdoptRegionSpawn(root);
			for (auto& script : created->scripts) m_spawned_scripts.push_back(std::move(script));
			return root;
		});
#if defined(AE_WEB_MANAGED)
		BindManagedBrowserServices(m_services.get());
		m_services->Register(ManagedBrowserSceneContext());
		auto lookup = [this](Entity entity, std::uint32_t index) { return GetInstanceHandle(entity, index); };
		m_network->SetFieldBridge(std::make_unique<net::BrowserScriptFieldBridge>(ManagedBrowserApi(), m_host, lookup));
		m_network->SetRpcBridge(std::make_unique<net::BrowserRpcBridge>(ManagedBrowserApi(), m_host, lookup, *m_services));
#endif
		m_world.RegisterSystem(std::make_unique<net::NetworkReceiveSystem>(*m_network));
		m_scene = std::make_unique<WebScene>(m_world, m_graphics);
		if (EM_ASM_INT({ return !!globalThis.aetherProject?.streamingManifest; }))
		{
			WebStreamingCallbacks callbacks;
			callbacks.bindBudget = [this](std::shared_ptr<streaming::BudgetLedger> budget) { m_streaming_budget = budget; m_scene->SetStreamingBudget(std::move(budget)); };
			callbacks.attach = [this](std::string_view id, std::string_view reference, std::span<const std::byte> preparedSnapshot, std::uint64_t gateNode) -> Expected<std::optional<WebRegion>>
			{
				const std::string key(id);
				auto it = m_activating_regions.find(key);
				if (it == m_activating_regions.end())
				{
					AE_TRY(activation, preparedSnapshot.empty() ? m_scene->BeginRegion(reference) : m_scene->BeginPreparedRegion(preparedSnapshot, gateNode));
					activation->region.id = key;
					it = m_activating_regions.emplace(key, std::move(*activation)).first;
				}
				if (!m_activation_remaining) return std::optional<WebRegion>{};
				const auto allowance = std::exchange(m_activation_remaining, 0u);
				auto step = m_scene->StepRegion(it->second, allowance);
				if (!step)
				{
					m_scene->CancelRegion(it->second);
					m_activating_regions.erase(it);
					return std::unexpected(step.error());
				}
				if (!*step) return std::optional<WebRegion>{};
				auto region = std::move(it->second.region);
				m_activating_regions.erase(it);
				for (const auto entity: region.entities)
				{
					m_world.EmplaceOrReplace<scene::StreamingRegionOwner>(entity, scene::StreamingRegionOwner{key});
					if (!m_world.Has<SceneNodeComponent>(entity))
						m_world.Emplace<SceneNodeComponent>(entity, SceneNodeComponent{scene::GenerateSceneNodeId()});
				}
				return std::optional<WebRegion>{std::move(region)};
			};
			callbacks.cancelAttach = [this](std::string_view id)
			{
				auto it = m_activating_regions.find(std::string(id));
				if (it != m_activating_regions.end()) { m_scene->CancelRegion(it->second); m_activating_regions.erase(it); }
			};
			callbacks.start = [this](WebRegion& region) { return StepRegionScripts(region); };
			callbacks.commit = [this](WebRegion& region)
			{
				if (region.activationGate.IsValid() && m_world.GetRegistry().valid(World::ToEntt(region.activationGate)))
					m_world.Remove<DisabledComponent>(region.activationGate);
			};
			callbacks.detach = [this](WebRegion& region) { return DetachRegion(region); };
			callbacks.ready = [this] { return !m_scene->StreamingPending() && !m_scene->StreamingBudgetBlocked() && !m_graphics.StreamingUploadsPending() && !m_graphics.StreamingUploadBlocked(); };
			callbacks.budgetBlocked = [this] { return m_scene->StreamingBudgetBlocked() || m_graphics.StreamingUploadBlocked(); };
			callbacks.capture = [this](const WebRegion& region) { return CaptureRegion(region); };
			callbacks.restore = [this](WebRegion& region, std::string_view state) { return RestoreRegion(region, state); };
			callbacks.restoreScripts = [this](WebRegion& region, std::string_view state)
			{
				std::size_t remaining = m_activation_remaining;
				auto result = StepRegionScriptRestore(region, state, remaining);
				m_activation_remaining = static_cast<std::uint32_t>(remaining);
				return result;
			};
			callbacks.promotedAlive = [this](const std::vector<Entity>& entities)
			{
				return std::any_of(entities.begin(), entities.end(), [this](Entity entity) { return m_world.GetRegistry().valid(World::ToEntt(entity)); });
			};
			callbacks.persistentRoots = [this](std::string_view region)
			{
				std::vector<Entity> roots;
				auto view = m_world.GetRegistry().view<scene::StreamingRegionOwner>();
				for (const auto raw: view)
				{
					const auto entity = World::FromEntt(raw);
					if (view.get<scene::StreamingRegionOwner>(raw).region != region || !ecs::HasDontDestroyOnLoadAncestor(m_world, entity)) continue;
					const auto* hierarchy = m_world.TryGet<HierarchyComponent>(entity);
					if (!hierarchy || !hierarchy->parent.IsValid() || !ecs::HasDontDestroyOnLoadAncestor(m_world, hierarchy->parent))
						roots.push_back(entity);
				}
				return roots;
			};
			callbacks.persistentAssets = [this](Entity root) { return m_scene->CapturePersistentAssets(root); };
			callbacks.measurePersistentAssets = [this](Entity root) { return m_scene->MeasurePersistentAssets(root); };
			callbacks.retirementPending = [this] { return m_graphics.MeshRetirementPending() || m_graphics.TextureRetirementPending(); };
			AE_TRY(backend, CreateWebStreamingBackend("streaming-manifest.json", std::move(callbacks)));
			m_streaming = std::make_unique<scene::WorldStreaming>(std::move(*backend));
#if defined(AE_WEB_MANAGED)
			ManagedBrowserSceneContext().streaming = m_streaming.get();
#endif
			EngineSettings::Streaming config;
			config.cpuBudgetMiB = EM_ASM_INT({ return aetherProject.streaming?.cpuBudgetMiB ?? $0; }, config.cpuBudgetMiB);
			config.gpuBudgetMiB = EM_ASM_INT({ return aetherProject.streaming?.gpuBudgetMiB ?? $0; }, config.gpuBudgetMiB);
			config.stagingBudgetMiB = EM_ASM_INT({ return aetherProject.streaming?.stagingBudgetMiB ?? $0; }, config.stagingBudgetMiB);
			config.uploadBudgetKiBPerFrame = EM_ASM_INT({ return aetherProject.streaming?.uploadBudgetKiBPerFrame ?? $0; }, config.uploadBudgetKiBPerFrame);
			config.maxPendingRequests = EM_ASM_INT({ return aetherProject.streaming?.maxPendingRequests ?? $0; }, config.maxPendingRequests);
			config.activationEntitiesPerFrame = EM_ASM_INT({ return aetherProject.streaming?.activationEntitiesPerFrame ?? $0; }, config.activationEntitiesPerFrame);
			const auto budgets = StreamingBudgetsFromSettings(config);
			if (!budgets) AE_UNEXPECTED(AetherError::Asset("Invalid browser streaming budgets"));
			m_activation_budget = budgets->activationEntitiesPerFrame;
			AE_TRY_VOID(m_streaming->SetBudgets(*budgets));
		}
		m_world.RegisterSystem(std::make_unique<audio::AudioSystem>(*m_services));
		m_world.RegisterSystem(std::make_unique<net::NetworkSendSystem>(*m_network));
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
		if (m_streaming) m_streaming->ResetWorld();
		m_starting_regions.clear();
		m_script_restores.clear();
		m_world.UnregisterSystem("AudioSystem");
		m_audio->StopAllVoices();
		m_world.RegisterSystem(std::make_unique<audio::AudioSystem>(*m_services));
		for (auto it = m_instances.begin(); it != m_instances.end();)
		{
			if (m_world.GetRegistry().valid(World::ToEntt(it->entity)) && ecs::HasDontDestroyOnLoadAncestor(m_world, it->entity))
			{
				++it;
				continue;
			}
			const auto* scripts = m_world.GetRegistry().valid(World::ToEntt(it->entity)) ? m_world.TryGet<ScriptComponent>(it->entity) : nullptr;
			if (scripts && it->scriptIndex < scripts->scripts.size() && scripts->scripts[it->scriptIndex].attached)
				AE_TRY_VOID(m_host.Detach(it->handle));
			AE_TRY_VOID(m_host.Destroy(it->handle));
			it = m_instances.erase(it);
		}
		m_spawned_scripts.clear();
		m_custom_passes.registered.clear();
		m_custom_passes.frame.passes.clear();
		if (auto* pending = m_world.GetRegistry().ctx().find<scene::detail::StreamingScriptNodeReferences>())
		{
			std::unordered_set<std::uint64_t> persistent;
			for (auto&& [raw, node]: m_world.View<SceneNodeComponent>().each())
			{
				if (ecs::HasDontDestroyOnLoadAncestor(m_world, World::FromEntt(raw)))
				{
					persistent.insert(node.id);
				}
			}
			std::erase_if(pending->entries, [&](const auto& reference) { return !persistent.contains(reference.ownerNode); });
		}
		AE_TRY(records, m_scene->Load(name));
		if (m_streaming) AE_TRY_VOID(m_streaming->Configure(m_scene->StreamingRegions()));
#if defined(AE_WEB_MANAGED)
		// The shared C# interop exports act on this scene's physics and clip catalog.
		BindManagedBrowserSimulation(static_cast<PhysicsSystem*>(m_world.FindSystem("PhysicsSystem")), m_scene->Clips());
#endif
		m_manual_warm_frames = 0;
		if (!m_host_ready)
		{
			++m_generation;
#if defined(AE_WEB_MANAGED)
			AE_TRY_VOID(m_host.Reset(m_module, "csharp", m_generation));
#else
			AE_TRY_VOID(m_host.Reset(m_module, "rust", m_generation));
#endif
			m_host_ready = true;
		}
		if (deferScripts)
		{
			// Native creates and attaches a freshly loaded scene's scripts on the NEXT
			// ScriptComponentSystem update, never inside the update that requested the load.
			m_spawned_scripts = std::move(*records);
		}
		else
		{
			AE_TRY_VOID(StartScripts(*records));
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
			ActorScope actor(m_callback_entity, instance.entity);
#if defined(AE_WEB_MANAGED)
			const scripting::ActiveScriptEntityScope managedActor(ManagedBrowserSceneContext(), instance.entity);
#endif
			if (!m_instances[index].ownershipSettled)
			{
				const auto query = net::QueryOwnership(m_world, m_network.get(), instance.entity);
				if (query.known)
				{
					const bool changed = !m_instances[index].ownershipKnown || m_instances[index].isOwner != query.isOwner || m_instances[index].owner != query.owner;
					m_instances[index].ownershipKnown = true;
					m_instances[index].isOwner = query.isOwner;
					m_instances[index].owner = query.owner;
					m_instances[index].ownershipSettled = !query.replicated;
					if (changed && (m_host.Capabilities() & AE_GAMEPLAY_CAP_OWNERSHIP))
						AE_TRY_VOID(m_host.DispatchOwnershipChanged(instance.handle, query.owner, query.isOwner));
				}
			}
			if ((m_module.capabilities & AE_GAMEPLAY_CAP_NETWORK) != 0)
			{
				if (const auto* receive = static_cast<net::NetworkReceiveSystem*>(m_world.FindSystem("NetworkReceiveSystem")))
					for (const auto& event : receive->GameplayEvents())
						AE_TRY_VOID(m_host.DispatchNetwork(instance.handle, event.code, event.peer));
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
		return StartScripts(std::exchange(m_spawned_scripts, {}));
	}

	Expected<void> WebGameplay::StartScripts(const std::vector<WebScriptRecord>& records)
	{
		for (const auto& record: records)
		{
			if (!m_world.GetRegistry().valid(World::ToEntt(record.entity)))
			{
				continue;
			}
			AE_TRY(handle, m_host.CreateInstance(record.record.type, record.entity.id));
			EnablePhysicsEvents(record.entity);
			m_instances.push_back({record.entity, *handle, record.record.type, record.scriptIndex});
			AE_TRY_VOID(m_host.TryApplyProperties(*handle, record.record.type, record.record.properties));
			RetainStreamingScriptReferences(record);
		}
		for (const auto& record: records)
		{
			const auto handle = GetInstanceHandle(record.entity, record.scriptIndex);
			if (handle == 0)
				continue;
			ActorScope actor(m_callback_entity, record.entity);
#if defined(AE_WEB_MANAGED)
			const scripting::ActiveScriptEntityScope managedActor(ManagedBrowserSceneContext(), record.entity);
#endif
			AE_TRY_VOID(m_host.Attach(handle));
			if (auto* scripts = m_world.TryGet<ScriptComponent>(record.entity); scripts && record.scriptIndex < scripts->scripts.size())
				scripts->scripts[record.scriptIndex].attached = true;
		}
		return {};
	}

	Expected<bool> WebGameplay::StepRegionScripts(WebRegion& region)
	{
		auto& progress = m_starting_regions[region.id];
		while (m_activation_remaining)
		{
			if (progress.cursor == region.scripts.size())
			{
				if (!progress.attaching) { progress.cursor = 0; progress.attaching = true; continue; }
				m_starting_regions.erase(region.id);
				return true;
			}
			const auto& record = region.scripts[progress.cursor++];
			--m_activation_remaining;
			if (!m_world.GetRegistry().valid(World::ToEntt(record.entity)))
				AE_UNEXPECTED(AetherError::Engine("Streaming script entity disappeared during activation"));
			ActorScope actor(m_callback_entity, record.entity);
#if defined(AE_WEB_MANAGED)
			const scripting::ActiveScriptEntityScope managedActor(ManagedBrowserSceneContext(), record.entity);
#endif
			if (!progress.attaching)
			{
				AE_TRY(handle, m_host.CreateInstance(record.record.type, record.entity.id));
				EnablePhysicsEvents(record.entity);
				m_instances.push_back({record.entity, *handle, record.record.type, record.scriptIndex});
				AE_TRY_VOID(m_host.TryApplyProperties(*handle, record.record.type, record.record.properties));
				RetainStreamingScriptReferences(record);
			}
			else
			{
				const auto handle = GetInstanceHandle(record.entity, record.scriptIndex);
				if (!handle) AE_UNEXPECTED(AetherError::Engine("Streaming script instance disappeared during activation"));
				AE_TRY_VOID(m_host.Attach(handle));
				if (auto* scripts = m_world.TryGet<ScriptComponent>(record.entity); scripts && record.scriptIndex < scripts->scripts.size())
					scripts->scripts[record.scriptIndex].attached = true;
			}
		}
		return false;
	}

	void WebGameplay::AdoptRegionSpawn(Entity root)
	{
		Entity actor = m_callback_entity;
#if defined(AE_WEB_MANAGED)
		if (ManagedBrowserSceneContext().activeScriptEntity.IsValid())
			actor = ManagedBrowserSceneContext().activeScriptEntity;
#endif
		if (!actor.IsValid() || !m_world.GetRegistry().valid(World::ToEntt(actor))) return;
		const auto* owner = m_world.TryGet<scene::StreamingRegionOwner>(actor);
		if (!owner) return;
		const std::string region = owner->region;
		Entity gate = actor;
		for (;;)
		{
			const auto* hierarchy = m_world.TryGet<HierarchyComponent>(gate);
			if (!hierarchy || !hierarchy->parent.IsValid()) break;
			const auto* parentOwner = m_world.TryGet<scene::StreamingRegionOwner>(hierarchy->parent);
			if (!parentOwner || parentOwner->region != region) break;
			gate = hierarchy->parent;
		}
		if (gate != root) ecs::SetParent(m_world, root, gate);
		std::vector<Entity> queue{root};
		for (std::size_t i = 0; i < queue.size(); ++i)
		{
			const auto entity = queue[i];
			if (!m_world.GetRegistry().valid(World::ToEntt(entity))) continue;
			m_world.EmplaceOrReplace<scene::StreamingRegionOwner>(entity, scene::StreamingRegionOwner{region});
			auto* node = m_world.TryGet<SceneNodeComponent>(entity);
			if (!node) node = &m_world.Emplace<SceneNodeComponent>(entity, SceneNodeComponent{scene::GenerateSceneNodeId()});
			if (!node->id) node->id = scene::GenerateSceneNodeId();
			if (const auto* hierarchy = m_world.TryGet<HierarchyComponent>(entity))
				queue.insert(queue.end(), hierarchy->children.begin(), hierarchy->children.end());
		}
	}


	void WebGameplay::RetainStreamingScriptReferences(const WebScriptRecord& record)
	{
		const auto* owner = m_world.TryGet<SceneNodeComponent>(record.entity);
		if (!owner || !owner->id)
		{
			return;
		}
		auto& context = m_world.GetRegistry().ctx();
		auto& pending = context.contains<scene::detail::StreamingScriptNodeReferences>()
		    ? context.get<scene::detail::StreamingScriptNodeReferences>()
		    : context.emplace<scene::detail::StreamingScriptNodeReferences>();
		for (const auto& [name, target]: record.record.nodeRefs)
		{
			const auto value = record.record.properties.find(name);
			if (value == record.record.properties.end() || value->second.i64 != 0 || !target)
			{
				continue;
			}
			std::erase_if(pending.entries, [&](const auto& ref)
			{
				return ref.ownerNode == owner->id && ref.scriptIndex == record.scriptIndex && ref.property == name;
			});
			pending.entries.push_back({owner->id, target, record.scriptIndex, record.record.type, name, 0});
		}
	}

	Expected<void> WebGameplay::PrepareStreamingScriptReferences(const std::vector<Entity>& entities)
	{
		std::unordered_set<std::uint32_t> detached;
		for (const auto entity: entities)
		{
			detached.insert(entity.id);
		}
		struct Change
		{
			Entity owner;
			std::size_t index;
			std::string name;
			std::uint64_t ownerNode, targetNode;
		};
		std::vector<Change> changes;
		for (auto&& [raw, component]: m_world.View<ScriptComponent>().each())
		{
			const Entity owner = World::FromEntt(raw);
			if (detached.contains(owner.id))
			{
				continue;
			}
			for (std::size_t index = 0; index < component.scripts.size(); ++index)
			{
				auto& script = component.scripts[index];
				const auto handle = GetInstanceHandle(owner, static_cast<std::uint32_t>(index));
				if (handle)
				{
					const auto properties = m_host.BorrowScriptProperties(script.path);
					for (std::size_t property = 0; property < properties.size(); ++property)
					{
						if (properties[property].type != ScriptPropertyValue::Type::Entity && properties[property].type != ScriptPropertyValue::Type::Component)
						{
							continue;
						}
						ScriptPropertyValue value;
						value.type = properties[property].type;
						if (!m_host.GetPropertyValue(handle, static_cast<int>(property), value))
						{
							AE_UNEXPECTED(AetherError::Asset("Cannot inspect browser streaming script reference"));
						}
						script.properties[properties[property].name] = std::move(value);
					}
				}
				for (const auto& [name, value]: script.properties)
				{
					if ((value.type != ScriptPropertyValue::Type::Entity && value.type != ScriptPropertyValue::Type::Component)
					    || value.i64 <= 0 || value.i64 > UINT32_MAX || !detached.contains(static_cast<std::uint32_t>(value.i64)))
					{
						continue;
					}
					const auto* source = m_world.TryGet<SceneNodeComponent>(owner);
					const auto* target = m_world.TryGet<SceneNodeComponent>(Entity{static_cast<std::uint32_t>(value.i64)});
					if (!source || !source->id || !target || !target->id)
					{
						AE_UNEXPECTED(AetherError::Asset("Browser streaming reference has no stable node"));
					}
					changes.push_back({owner, index, name, source->id, target->id});
				}
			}
		}
		auto& context = m_world.GetRegistry().ctx();
		auto& pending = context.contains<scene::detail::StreamingScriptNodeReferences>()
		    ? context.get<scene::detail::StreamingScriptNodeReferences>()
		    : context.emplace<scene::detail::StreamingScriptNodeReferences>();
		for (const auto& change: changes)
		{
			auto& script = m_world.Get<ScriptComponent>(change.owner).scripts[change.index];
			auto value = script.properties.at(change.name);
			value.i64 = 0;
			const auto handle = GetInstanceHandle(change.owner, static_cast<std::uint32_t>(change.index));
			if (handle)
			{
				const int property = m_host.FindPropertyIndex(script.path, change.name);
				if (property < 0)
				{
					AE_UNEXPECTED(AetherError::Asset("Browser streaming reference property disappeared"));
				}
				AE_TRY_VOID(m_host.TrySetPropertyValue(handle, property, value));
			}
			script.properties[change.name] = std::move(value);
			std::erase_if(pending.entries, [&](const auto& ref)
			{
				return ref.ownerNode == change.ownerNode && ref.scriptIndex == change.index && ref.property == change.name;
			});
			pending.entries.push_back({change.ownerNode, change.targetNode, change.index, script.path, change.name, 0});
		}
		return {};
	}

	Expected<void> WebGameplay::ResolveStreamingScriptReferences()
	{
		auto* pending = m_world.GetRegistry().ctx().find<scene::detail::StreamingScriptNodeReferences>();
		if (!pending || pending->entries.empty())
		{
			return {};
		}
		const scene::StreamingNodeIndex nodes(m_world);
		for (auto it = pending->entries.begin(); it != pending->entries.end();)
		{
			const auto owner = nodes.exact.find(it->ownerNode);
			if (owner == nodes.exact.end())
			{
				++it;
				continue;
			}
			auto* scripts = m_world.TryGet<ScriptComponent>(owner->second);
			if (!scripts || it->scriptIndex >= scripts->scripts.size() || scripts->scripts[it->scriptIndex].path != it->scriptType)
			{
				it = pending->entries.erase(it);
				continue;
			}
			auto& script = scripts->scripts[it->scriptIndex];
			auto property = script.properties.find(it->property);
			if (property == script.properties.end())
			{
				it = pending->entries.erase(it);
				continue;
			}
			const auto handle = GetInstanceHandle(owner->second, static_cast<std::uint32_t>(it->scriptIndex));
			const int index = handle ? m_host.FindPropertyIndex(script.path, it->property) : -1;
			if (handle && (index < 0 || !m_host.GetPropertyValue(handle, index, property->second)))
			{
				AE_UNEXPECTED(AetherError::Asset("Cannot inspect deferred browser streaming reference"));
			}
			if (property->second.i64 != it->expected)
			{
				it = pending->entries.erase(it);
				continue;
			}
			AE_TRY(target, nodes.Resolve(it->targetNode));
			if (!target->IsValid())
			{
				++it;
				continue;
			}
			auto value = property->second;
			value.i64 = target->id;
			if (handle)
			{
				AE_TRY_VOID(m_host.TrySetPropertyValue(handle, index, value));
			}
			property->second = std::move(value);
			it = pending->entries.erase(it);
		}
		return {};
	}

	Expected<void> WebGameplay::DetachRegion(WebRegion& region)
	{
		std::vector<Entity> owned;
		auto view = m_world.GetRegistry().view<scene::StreamingRegionOwner>();
		for (const auto raw: view)
		{
			if (view.get<scene::StreamingRegionOwner>(raw).region == region.id)
				owned.push_back(World::FromEntt(raw));
		}
		for (const auto entity: owned)
		{
			if (!m_world.GetRegistry().valid(World::ToEntt(entity))) continue;
			if (ecs::HasDontDestroyOnLoadAncestor(m_world, entity))
			{
				const auto* hierarchy = m_world.TryGet<HierarchyComponent>(entity);
				if (hierarchy && hierarchy->parent.IsValid() && !ecs::HasDontDestroyOnLoadAncestor(m_world, hierarchy->parent))
					ecs::DetachFromParent(m_world, entity);
				region.promoted.push_back(entity);
				m_world.Remove<scene::StreamingRegionOwner>(entity);
			}
		}
		std::vector<Entity> detached;
		for (const auto entity: owned)
		{
			if (const auto* owner = m_world.TryGet<scene::StreamingRegionOwner>(entity); owner && owner->region == region.id)
			{
				detached.push_back(entity);
			}
		}
		AE_TRY_VOID(PrepareStreamingScriptReferences(detached));
		AE_TRY_VOID(scene::PrepareStreamingFieldReferencesForDetach(m_world, detached));
		for (auto it = m_instances.begin(); it != m_instances.end();)
		{
			const auto* owner = m_world.TryGet<scene::StreamingRegionOwner>(it->entity);
			if (owner && owner->region == region.id)
			{
				const auto* scripts = m_world.TryGet<ScriptComponent>(it->entity);
				if (scripts && it->scriptIndex < scripts->scripts.size() && scripts->scripts[it->scriptIndex].attached)
					AE_TRY_VOID(m_host.Detach(it->handle));
				AE_TRY_VOID(m_host.Destroy(it->handle));
				it = m_instances.erase(it);
			}
			else ++it;
		}
		m_starting_regions.erase(region.id);
		m_script_restores.erase(region.id);
		region.entities.clear();
		for (const auto entity: owned)
			if (m_world.GetRegistry().valid(World::ToEntt(entity)) && m_world.Has<scene::StreamingRegionOwner>(entity))
				region.entities.push_back(entity);
		std::unordered_set<std::uint64_t> detachedNodes;
		for (const auto entity: detached)
		{
			if (const auto* node = m_world.TryGet<SceneNodeComponent>(entity))
			{
				detachedNodes.insert(node->id);
			}
		}
		if (auto* pending = m_world.GetRegistry().ctx().find<scene::detail::StreamingScriptNodeReferences>())
		{
			std::erase_if(pending->entries, [&](const auto& reference) { return detachedNodes.contains(reference.ownerNode); });
		}
		scene::ForgetStreamingFieldReferences(m_world, detached);
		m_scene->DetachRegion(region);
		return {};
	}

	Expected<void> WebGameplay::ProcessPendingDestroys()
	{
#if defined(AE_WEB_MANAGED)
		auto& managedPending = ManagedBrowserSceneContext().pendingDestroys;
		m_pending_destroys.insert(m_pending_destroys.end(), managedPending.begin(), managedPending.end());
		managedPending.clear();
#endif
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
		if (m_streaming)
		{
			const bool cameraSource = EM_ASM_INT({ return aetherProject.streaming?.defaultCameraSource !== false; });
			auto cameras = m_world.GetRegistry().view<MainCameraComponent, TransformComponent>();
			if (cameraSource && !m_scene->StreamingRegions().empty() && cameras.begin() != cameras.end())
			{
				const auto& transform = cameras.get<TransformComponent>(*cameras.begin());
				streaming::Source source;
				source.id = "__aether_default_camera";
				source.position = glm::vec3(transform.localToWorld[3]);
				source.loadRadius = static_cast<float>(EM_ASM_DOUBLE({ return aetherProject.streaming?.loadRadius ?? 128; }));
				source.prefetchRadius = std::max(source.loadRadius, static_cast<float>(EM_ASM_DOUBLE({ return aetherProject.streaming?.prefetchRadius ?? 256; })));
				(void)m_streaming->SetSource(std::move(source));
			}
			else m_streaming->RemoveSource("__aether_default_camera");
			m_graphics.PumpStreamingUploads();
			m_activation_remaining = m_activation_budget;
			m_streaming->Update();
			AE_TRY_VOID(scene::ResolveStreamingFieldReferences(m_world));
			AE_TRY_VOID(ResolveStreamingScriptReferences());
			const auto diagnostics = m_streaming->GetDiagnosticsJson();
			EM_ASM({ try { aetherRuntime.streaming = JSON.parse(UTF8ToString($0)); } catch (_) { aetherRuntime.streaming = null; } }, diagnostics.c_str());
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
		const int textBytes = EM_ASM_INT({ return lengthBytesUTF8(aetherInput.typedText); });
		if (textBytes > 0)
		{
			std::string text(static_cast<std::size_t>(textBytes) + 1, '\0');
			EM_ASM({ stringToUTF8(aetherInput.typedText, $0, $1); aetherInput.typedText = ""; }, text.data(), text.size());
			text.resize(static_cast<std::size_t>(textBytes));
			m_input.SetSyntheticChars(text);
		}
		m_input.Update();
		m_input.SetBrowserMouseDelta(m_mouse_delta);
		m_input.SetBrowserScrollDelta(m_scroll_delta);
		m_input.UpdateCursorLock(canvasFocused);
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
		ui::UiTextBoxSystem::Update(m_world, m_input, &m_scene->Fonts(), static_cast<float>(now / 1000.0));
		EM_ASM({ aetherInput.pasteText = null; });
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
#if defined(AE_WEB_MANAGED)
				ManagedBrowserSceneContext().deltaTime = 1.0f / 60.0f;
#endif
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
		// The AudioWorklet only consumes transferred buffers. Mixing remains on
		// this thread, so native miniaudio never needs shared memory or locks.
		std::array<float, 2048> pcm{};
		while (EM_ASM_INT({ return aetherPlatform.audioBufferAvailable() ? 1 : 0; }))
		{
			m_audio->RenderFrames(pcm.data(), 1024);
			EM_ASM({ aetherPlatform.submitAudio(HEAPF32.subarray($0 >> 2, ($0 >> 2) + 2048)); }, pcm.data());
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
		if (!aether::scripting::IsGameplayUtf8(request.text) || (request.operation != AE_GAMEPLAY_SERVICE_STREAMING_SET_SOURCE && !aether::scripting::gameplay3d::HasRawText2(request.operation) && !aether::scripting::IsGameplayUtf8(request.text2)))
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
		const bool networkEntityOperation = request.operation >= AE_GAMEPLAY_SERVICE_NET_DESPAWN && request.operation <= AE_GAMEPLAY_SERVICE_NET_RELEASE_OWNERSHIP;
		if (entityOperation || networkEntityOperation)
		{
			std::uint32_t valid = 0;
			const auto status = engine.entityValid(engine.context, request.entity, &valid);
			if (status != AE_GAMEPLAY_OK)
			{
				return status;
			}
		}
		if (request.operation >= AE_GAMEPLAY_SERVICE_NET_HOST && request.operation <= AE_GAMEPLAY_SERVICE_NET_DISCONNECT_REASON)
			return aether::app::scripting::ExecuteGameplayNetworkService(m_network.get(), m_world, m_access, request, value, buffer, m_pending_destroys);
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
			case AE_GAMEPLAY_SERVICE_STREAMING_SET_SOURCE:
			{
				if (!m_streaming) return AE_GAMEPLAY_UNSUPPORTED;
				if (request.integer < 0 || request.integer > 3 || request.text2.length != sizeof(float) || !request.text2.data) return AE_GAMEPLAY_INVALID_ARGUMENT;
				float prefetch;
				std::memcpy(&prefetch, request.text2.data, sizeof(prefetch));
				streaming::Source source{text, {request.vector[0], request.vector[1], request.vector[2]}, request.vector[3], prefetch, static_cast<io::IOPriority>(request.integer)};
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = m_streaming->SetSource(std::move(source)) ? 1 : 0;
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_STREAMING_REMOVE_SOURCE:
			case AE_GAMEPLAY_SERVICE_STREAMING_CANCEL:
			case AE_GAMEPLAY_SERVICE_STREAMING_RELEASE:
				if (!m_streaming) return AE_GAMEPLAY_UNSUPPORTED;
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				if (request.operation == AE_GAMEPLAY_SERVICE_STREAMING_REMOVE_SOURCE) value.integer = m_streaming->RemoveSource(text);
				else if (request.operation == AE_GAMEPLAY_SERVICE_STREAMING_CANCEL) value.integer = m_streaming->Cancel(static_cast<std::uint64_t>(request.integer));
				else value.integer = m_streaming->Release(static_cast<std::uint64_t>(request.integer));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_STREAMING_REQUEST:
				if (!m_streaming) return AE_GAMEPLAY_UNSUPPORTED;
				if (request.integer < 0 || request.integer > 3) return AE_GAMEPLAY_INVALID_ARGUMENT;
				value.type = AE_GAMEPLAY_PROPERTY_INT;
				value.integer = static_cast<std::int64_t>(m_streaming->Request(text, request.vector[0] != 0, static_cast<io::IOPriority>(request.integer)));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_STREAMING_STATE:
				if (!m_streaming) return AE_GAMEPLAY_UNSUPPORTED;
				value.type = AE_GAMEPLAY_PROPERTY_INT;
				value.integer = static_cast<std::int64_t>(m_streaming->GetRequestState(static_cast<std::uint64_t>(request.integer)));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_STREAMING_DIAGNOSTICS:
			{
				if (!m_streaming) return AE_GAMEPLAY_UNSUPPORTED;
				const auto json = m_streaming->GetDiagnosticsJson();
				value.type = AE_GAMEPLAY_PROPERTY_STRING;
				return aether::scripting::CopyGameplayUtf8({json.data(), static_cast<std::uint32_t>(json.size())}, buffer);
			}
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
				AdoptRegionSpawn(created->root);
#if defined(AE_WEB_MANAGED)
				// Predicted C# payloads exist only during this call. Ordinary prefab
				// instantiation remains deferred until the next script tick.
				if (request.integer == 1)
				{
					for (const Entity entity: created->entities)
						net::MarkPredicted(m_world, entity);
					aether::scripting::GameplayAccess::PredictedAttachScope nestedAttach(m_access);
					if (!StartScripts(created->scripts))
						return AE_GAMEPLAY_SCRIPT_ERROR;
				}
				else
#endif
				{
					m_spawned_scripts.insert(m_spawned_scripts.end(), std::make_move_iterator(created->scripts.begin()), std::make_move_iterator(created->scripts.end()));
				}
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.entity = m_access.Handle(created->root);
				return AE_GAMEPLAY_OK;
			}
			case AE_GAMEPLAY_SERVICE_PLAY_AUDIO:
			{
				const auto voice = m_audio->Play2D(text);
				value.type = AE_GAMEPLAY_PROPERTY_INT;
				value.integer = voice.id;
				if (voice.IsValid())
				{
					EM_ASM({ ++aetherPlatform.report.scheduledSounds; });
					return AE_GAMEPLAY_OK;
				}
				return AE_GAMEPLAY_SCRIPT_ERROR;
			}
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
				AdoptRegionSpawn(created);
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
				const bool writesBody = request.operation == AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D ||
				        request.operation == AE_GAMEPLAY_SERVICE_ADD_FORCE_3D || request.operation == AE_GAMEPLAY_SERVICE_ADD_IMPULSE_3D ||
				        request.operation == AE_GAMEPLAY_SERVICE_CHARACTER_MOVE || request.operation == AE_GAMEPLAY_SERVICE_CHARACTER_JUMP;
				if (writesBody && m_network && !m_network->HasAuthority(m_world, entity))
					return AE_GAMEPLAY_NO_AUTHORITY;
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
