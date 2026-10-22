#pragma once

#include <array>
#include <string>
#include <unordered_set>
#include "platform/Input.hpp"
#include "web/WebScene.hpp"
#include "scripting/GameplayHost.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayServices.h"
#include "rendering/CustomPassRegistry.hpp"

namespace aether::app::web
{
	class WebGameplay
	{
	public:
		WebGameplay(World& world, aether::scripting::GameplayAccess& access, webgpu::WebGpuDevice& graphics, std::array<bool, 512>& keys, std::array<bool, 512>& pressedKeys, const AetherGameplayModuleApi& module);
		~WebGameplay();
		Expected<void> Initialize();
		Expected<void> Frame(double now, bool& capture);
		AetherGameplayStatus Invoke(const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& result, AetherGameplayUtf8Buffer buffer);
		void Stop();

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
		};

		Expected<void> Load(const std::string& name, bool deferScripts = false);
		Expected<void> RunScripts(float dt);
		Expected<void> ProcessPendingDestroys();
		Expected<void> StartSpawnedScripts();
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
