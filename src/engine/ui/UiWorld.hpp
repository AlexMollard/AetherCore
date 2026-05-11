#pragma once

#include <entt/entt.hpp>
#include <utility>

#include "scene/Entity.hpp"
#include "UiComponents.hpp"

namespace aether::ui
{
	class UiEntityHandle;

	// Lightweight entt-backed ECS world for UI entities only.
	// Separate from the game World so UI entities never participate in 3D
	// rendering (FlushToQueue, RenderQueue, etc.).
	class UiWorld
	{
	public:
		using Registry = entt::registry;

		// ── Entity lifecycle ──────────────────────────────────────────────────
		[[nodiscard]] Entity Create();
		void Destroy(Entity entity);
		[[nodiscard]] UiEntityHandle Spawn();

		// ── Component helpers ─────────────────────────────────────────────────
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

		// ── Conversions (public so UiSystem can use them during iteration) ────
		[[nodiscard]] static entt::entity ToEntt(Entity entity) noexcept;
		[[nodiscard]] static Entity FromEntt(entt::entity entity) noexcept;

	private:
		Registry m_registry;
	};

	// ── Fluent entity handle ──────────────────────────────────────────────────
	class UiEntityHandle
	{
	public:
		UiEntityHandle(UiWorld& world, Entity entity)
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

		operator Entity() const noexcept
		{
			return m_entity;
		}

		void Destroy()
		{
			m_world.Destroy(m_entity);
		}

		template<typename T, typename... Args>
		UiEntityHandle& Add(Args&&... args)
		{
			m_world.Emplace<T>(m_entity, std::forward<Args>(args)...);
			return *this;
		}

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
		bool Has() const
		{
			return m_world.Has<T>(m_entity);
		}

	private:
		UiWorld& m_world;
		Entity m_entity;
	};

	inline UiEntityHandle UiWorld::Spawn()
	{
		return UiEntityHandle{ *this, Create() };
	}

} // namespace aether::ui
