#pragma once

#include <memory>
#include <vector>

#include "scene/SceneKind.hpp"

namespace aether
{
	class World;

	class System
	{
	public:
		virtual ~System() = default;

		virtual void OnRegister([[maybe_unused]] World& world)
		{
		}

		virtual void Update(World& world, float dt) = 0;

		virtual void OnUnregister([[maybe_unused]] World& world)
		{
		}

		[[nodiscard]] virtual const char* GetName() const
		{
			return "System";
		}

	};

	class SystemRegistry
	{
	public:
		void Register(std::unique_ptr<System> system);

		void Unregister(const char* name);

		void UpdateAll(World& world, float dt);

		System* Find(const char* name);

		// Calls OnUnregister on every system (reverse registration order) and
		// destroys them. Must run while the World's registry is still alive so
		// systems can sever entt signal connections safely.
		void Shutdown(World& world);

	private:
		std::vector<std::unique_ptr<System>> m_systems;
	};
} // namespace aether
