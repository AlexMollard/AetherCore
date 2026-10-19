#pragma once

#include <array>
#include "platform/Input.hpp"
#include "web/WebScene.hpp"
#include "scripting/GameplayHost.hpp"
#include "scripting/GameplayAccess.hpp"
#include "scripting/GameplayServices.h"

namespace aether::app::web
{
	class WebGameplay
	{
	public:
		WebGameplay(World& world, aether::scripting::GameplayAccess& access, webgpu::WebGpuDevice& graphics, std::array<bool, 512>& keys, std::array<bool, 512>& pressedKeys, const AetherGameplayModuleApi& module);
		~WebGameplay();
		Expected<void> Initialize();
		Expected<void> Frame(double now, bool capture);
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
		};

		Expected<void> Load(const std::string& name);
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
		std::vector<Entity> m_pending_destroys;
		std::string m_pending_scene;
		std::uint32_t m_generation = 0, m_ticks = 0, m_frames = 0;
		double m_previous = 0, m_accumulator = 0;
	};
} // namespace aether::app::web
