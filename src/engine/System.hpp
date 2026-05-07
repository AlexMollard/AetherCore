#pragma once

#include <memory>
#include <vector>

namespace aether
{
	class World;

	// Base class for game systems that operate on the ECS world.
	// Systems are updated each frame by the world in registration order.
	class System
	{
	public:
		virtual ~System() = default;

		// Called once when the system is registered with the world.
		virtual void OnRegister([[maybe_unused]] World& world)
		{
		}

		// Called each frame to update this system. dt is the delta time in seconds.
		virtual void Update(World& world, float dt) = 0;

		// Called when the system is unregistered (optional cleanup).
		virtual void OnUnregister([[maybe_unused]] World& world)
		{
		}

		// Convenient name for debugging (optional).
		[[nodiscard]] virtual const char* GetName() const
		{
			return "System";
		}
	};

	// Type-erased holder for any system, with virtual dispatch.
	class SystemRegistry
	{
	public:
		// Register a system. Order matters - systems update in registration order.
		void Register(std::unique_ptr<System> system);

		// Unregister a system by name (if found).
		void Unregister(const char* name);

		// Update all registered systems.
		void UpdateAll(World& world, float dt);

		// Clear all systems.
		void Clear();

	private:
		std::vector<std::unique_ptr<System>> m_systems;
	};
} // namespace aether
