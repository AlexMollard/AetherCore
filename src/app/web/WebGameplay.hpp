#pragma once

#include <array>
#include <map>
#include <string>
#include <unordered_set>
#include "platform/Input.hpp"
#include "web/WebScene.hpp"
#include "scripting/GameplayHost.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayServices.h"
#include "rendering/CustomPassRegistry.hpp"
#include "scene/WorldStreaming.hpp"

namespace aether
{
	class ServiceContainer;
	namespace audio { class AudioSubsystem; }
	namespace net { class NetworkContext; }
}

namespace aether::app::web
{
	struct StreamingScriptRestoreCursor;
	class WebGameplay
	{
	public:
		WebGameplay(World& world, aether::scripting::GameplayAccess& access, webgpu::WebGpuDevice& graphics, std::array<bool, 512>& keys, std::array<bool, 512>& pressedKeys, const AetherGameplayModuleApi& module);
		~WebGameplay();
		Expected<void> Initialize();
		Expected<void> Frame(double now, bool& capture);
		AetherGameplayStatus Invoke(const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& result, AetherGameplayUtf8Buffer buffer);
		void Stop();
		std::uint64_t GetInstanceHandle(Entity entity, std::uint32_t scriptIndex) const;
		scripting::GameplayHost& Host() { return m_host; }


		void ResetTiming()
		{
			m_previous = 0;
			m_accumulator = 0;
		}

	private:
		struct Instance
		{
			Entity entity;
			std::uint64_t handle;
			std::string type;
			std::uint32_t scriptIndex = 0;
			bool ownershipKnown = false;
			bool ownershipSettled = false;
			bool isOwner = false;
			std::uint32_t owner = 0;
		};

		Expected<void> Load(const std::string& name, bool deferScripts = false);
		Expected<void> RunScripts(float dt);
		Expected<void> ProcessPendingDestroys();
		Expected<void> StartSpawnedScripts();
		Expected<void> StartScripts(const std::vector<WebScriptRecord>& records);
		Expected<bool> StepRegionScripts(WebRegion& region);
		Expected<std::string> CaptureRegion(const WebRegion& region);
		Expected<void> RestoreRegion(WebRegion& region, std::string_view snapshot);
		Expected<bool> StepRegionScriptRestore(WebRegion& region, std::string_view snapshot, std::size_t& remaining);
		Expected<void> DetachRegion(WebRegion& region);
		void RetainStreamingScriptReferences(const WebScriptRecord& record);
		Expected<void> PrepareStreamingScriptReferences(const std::vector<Entity>& entities);
		Expected<void> ResolveStreamingScriptReferences();
		void AdoptRegionSpawn(Entity root);
		void EnablePhysicsEvents(Entity entity);
		std::vector<WebScriptRecord> m_spawned_scripts;
		Expected<void> m_script_status;
#if AE_WEB_TEST_BRIDGE
		// Read-only observable state (transforms, script properties) for native-vs-browser parity.
		void PublishTestState();
#endif
		World& m_world;
		aether::scripting::GameplayAccess& m_access;
		webgpu::WebGpuDevice& m_graphics;
		std::array<bool, 512>& m_keys;
		std::array<bool, 512>& m_pressed_keys;
		AetherGameplayModuleApi m_module;
		scripting::GameplayHost m_host;
		Input m_input;
		glm::vec2 m_mouse_delta{}, m_scroll_delta{};
		std::unique_ptr<ServiceContainer> m_services;
		std::unique_ptr<net::NetworkContext> m_network;
		std::unique_ptr<audio::AudioSubsystem> m_audio;
		std::unique_ptr<scene::WorldStreaming> m_streaming;
		bool m_host_ready = false;
		std::shared_ptr<streaming::BudgetLedger> m_streaming_budget;
		Entity m_callback_entity{};
		std::map<std::string, WebRegionActivation> m_activating_regions;
		struct RegionScriptStart { std::size_t cursor = 0; bool attaching = false; };
		std::map<std::string, RegionScriptStart> m_starting_regions;
		std::map<std::string, std::unique_ptr<StreamingScriptRestoreCursor>> m_script_restores;
		std::uint32_t m_activation_budget = 64;
		std::uint32_t m_activation_remaining = 0;
		std::unique_ptr<WebScene> m_scene;
		std::vector<Instance> m_instances;
		aether::CustomPassRegistry m_custom_passes;
		std::unordered_set<std::string> m_custom_shaders;
		std::vector<Entity> m_pending_destroys;
		std::string m_pending_scene;
		std::uint32_t m_generation = 0, m_ticks = 0, m_frames = 0;
		std::uint32_t m_manual_warm_frames = 0;
		unsigned m_last_render_width = 0, m_last_render_height = 0;
		bool m_manual_pending_frame = false, m_manual_pending_advance = false;
		double m_previous = 0, m_accumulator = 0;
	};
} // namespace aether::app::web
