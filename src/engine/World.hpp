#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <utility>

#include "Components.hpp"
#include "Entity.hpp"
#include "System.hpp"

namespace aether
{
	class RenderQueue;

	// Lightweight ECS world backed by entt::registry.
	//
	// Entities must have at minimum a PipelineComponent + MeshComponent +
	// TransformComponent to be emitted by FlushToQueue().  MaterialComponent is
	// optional - entities without it fall back to vertex colour in the shader.
	class World
	{
	public:
		using Registry = entt::registry;

		// ── Entity lifecycle ──────────────────────────────────────────────────
		[[nodiscard]] Entity Create();
		void Destroy(Entity entity);

		// ── Generic ENTT helpers ──────────────────────────────────────────────
		template<typename T, typename... Args>
		T& Emplace(Entity entity, Args&&... args)
		{
			return m_registry.emplace<T>(ToEntt(entity), std::forward<Args>(args)...);
		}

		template<typename T, typename... Args>
		T& EmplaceOrReplace(Entity entity, Args&&... args)
		{
			return m_registry.emplace_or_replace<T>(ToEntt(entity), std::forward<Args>(args)...);
		}

		template<typename T>
		T* TryGet(Entity entity)
		{
			return m_registry.try_get<T>(ToEntt(entity));
		}

		template<typename T>
		const T* TryGet(Entity entity) const
		{
			return m_registry.try_get<T>(ToEntt(entity));
		}

		template<typename T>
		T& Get(Entity entity)
		{
			return m_registry.get<T>(ToEntt(entity));
		}

		template<typename T>
		const T& Get(Entity entity) const
		{
			return m_registry.get<T>(ToEntt(entity));
		}

		template<typename T>
		bool Has(Entity entity) const
		{
			return m_registry.any_of<T>(ToEntt(entity));
		}

		template<typename T>
		void Remove(Entity entity)
		{
			m_registry.remove<T>(ToEntt(entity));
		}

		template<typename... Components>
		auto View()
		{
			return m_registry.view<Components...>();
		}

		template<typename... Components>
		auto View() const
		{
			return m_registry.view<Components...>();
		}

		[[nodiscard]] Registry& GetRegistry() noexcept
		{
			return m_registry;
		}

		[[nodiscard]] const Registry& GetRegistry() const noexcept
		{
			return m_registry;
		}

		// ── Engine-internal ───────────────────────────────────────────────────

		// Emits a DrawCommand for every entity that has Pipeline + Mesh + Transform.
		// MaterialComponent is used if present, otherwise albedoSlot = kNoTexture.
		void FlushToQueue(RenderQueue& queue) const;

		// ── Systems (game logic layers operating on the world) ──────────────────
		// Register a system to be updated each frame.
		void RegisterSystem(std::unique_ptr<System> system);
		// Remove a system by name.
		void UnregisterSystem(const char* name);
		// Update all registered systems (called by the game loop).
		void UpdateSystems(float dt);

	private:
		[[nodiscard]] static entt::entity ToEntt(Entity entity) noexcept;
		[[nodiscard]] static Entity FromEntt(entt::entity entity) noexcept;

		SystemRegistry m_systems;
		Registry m_registry;
	};
} // namespace aether
