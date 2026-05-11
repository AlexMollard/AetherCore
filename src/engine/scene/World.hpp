#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <utility>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/System.hpp"

namespace aether
{
	class RenderQueue;
	class EntityHandle;

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

		// Creates an entity and returns a handle for fluent component attachment.
		[[nodiscard]] EntityHandle Spawn();

		// Wraps an existing entity in a handle.
		[[nodiscard]] EntityHandle Handle(Entity entity);

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

	// Lightweight view over a (World, Entity) pair for fluent component management.
	// Implicitly converts to Entity so it can be stored in containers or passed to
	// existing APIs that expect a raw Entity.
	class EntityHandle
	{
	public:
		EntityHandle(World& world, Entity entity)
		      : m_world(world), m_entity(entity)
		{
		}

		[[nodiscard]] Entity entity() const noexcept
		{
			return m_entity;
		}

		[[nodiscard]] bool IsValid() const noexcept
		{
			return m_entity.IsValid();
		}

		// Implicit conversion so EntityHandle can be used anywhere Entity is expected.
		operator Entity() const noexcept
		{
			return m_entity;
		}

		void Destroy()
		{
			m_world.Destroy(m_entity);
		}

		// Add a component to the entity. Returns *this to allow optional chaining.
		template<typename T, typename... Args>
		EntityHandle& Add(Args&&... args)
		{
			m_world.Emplace<T>(m_entity, std::forward<Args>(args)...);
			return *this;
		}

		template<typename T, typename... Args>
		EntityHandle& AddOrReplace(Args&&... args)
		{
			m_world.EmplaceOrReplace<T>(m_entity, std::forward<Args>(args)...);
			return *this;
		}

		template<typename T>
		EntityHandle& Remove()
		{
			m_world.Remove<T>(m_entity);
			return *this;
		}

		// Component access - mirrors World::Get / TryGet / Has.
		template<typename T>
		T& Get()
		{
			return m_world.Get<T>(m_entity);
		}

		template<typename T>
		const T& Get() const
		{
			return m_world.Get<T>(m_entity);
		}

		template<typename T>
		T* TryGet()
		{
			return m_world.TryGet<T>(m_entity);
		}

		template<typename T>
		const T* TryGet() const
		{
			return m_world.TryGet<T>(m_entity);
		}

		template<typename T>
		bool Has() const
		{
			return m_world.Has<T>(m_entity);
		}

	private:
		World& m_world;
		Entity m_entity;
	};

	// Out-of-line definitions (both classes must be complete first).
	inline EntityHandle World::Spawn()
	{
		return EntityHandle{ *this, Create() };
	}

	inline EntityHandle World::Handle(Entity entity)
	{
		return EntityHandle{ *this, entity };
	}

} // namespace aether
