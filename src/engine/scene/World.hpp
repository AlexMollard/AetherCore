#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <utility>
#include <vector>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/SceneKind.hpp"
#include "scene/System.hpp"

namespace aether
{
	class World
	{
	public:
		using Registry = entt::registry;

		World();
		// Shuts systems down (OnUnregister) before the registry is destroyed:
		// m_systems is declared before m_registry, so without this the registry
		// dies first and system destructors would sever entt signal connections
		// into freed memory.
		~World();
		World(const World&) = delete;
		World& operator=(const World&) = delete;
		World(World&&) = delete;
		World& operator=(World&&) = delete;

		[[nodiscard]] Entity Create();
		void Destroy(Entity entity);

		template<typename T, typename... Args>
		decltype(auto) Emplace(Entity entity, Args&&... args)
		{
			return m_registry.emplace<T>(ToEntt(entity), std::forward<Args>(args)...);
		}

		template<typename T, typename... Args>
		decltype(auto) EmplaceOrReplace(Entity entity, Args&&... args)
		{
			return m_registry.emplace_or_replace<T>(ToEntt(entity), std::forward<Args>(args)...);
		}

		template<typename T>
		T* TryGet(Entity entity)
		{
			return m_registry.try_get<T>(ToEntt(entity));
		}

		template<typename T>
		[[nodiscard]] const T* TryGet(Entity entity) const
		{
			return m_registry.try_get<T>(ToEntt(entity));
		}

		template<typename T>
		T& Get(Entity entity)
		{
			return m_registry.get<T>(ToEntt(entity));
		}

		template<typename T>
		[[nodiscard]] const T& Get(Entity entity) const
		{
			return m_registry.get<T>(ToEntt(entity));
		}

		template<typename T>
		[[nodiscard]] bool Has(Entity entity) const
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
		[[nodiscard]] auto View() const
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

		void RegisterSystem(std::unique_ptr<System> system);
		void UnregisterSystem(const char* name);
		System* FindSystem(const char* name);
		void UpdateSystems(float dt);

		[[nodiscard]] const std::vector<Entity>& Roots() const noexcept
		{
			return m_rootOrder;
		}

		void RegisterRoot(Entity entity);
		void UnregisterRoot(Entity entity);
		void InsertRootAt(Entity entity, int index);

		[[nodiscard]] SceneKind GetSceneKind() const noexcept
		{
			return m_sceneKind;
		}

		void SetSceneKind(SceneKind kind) noexcept
		{
			m_sceneKind = kind;
		}

		[[nodiscard]] SceneFeatureFlags GetSceneFeatures() const noexcept
		{
			return m_sceneFeatures;
		}

		void SetSceneFeatures(SceneFeatureFlags features) noexcept
		{
			m_sceneFeatures = features;
		}

		[[nodiscard]] static entt::entity ToEntt(Entity entity) noexcept;
		[[nodiscard]] static Entity FromEntt(entt::entity entity) noexcept;

	private:
		SystemRegistry m_systems;
		Registry m_registry;
		std::vector<Entity> m_rootOrder;
		SceneKind m_sceneKind = SceneKind::Scene3D;
		SceneFeatureFlags m_sceneFeatures = DefaultSceneFeatures(SceneKind::Scene3D);
	};
} // namespace aether
