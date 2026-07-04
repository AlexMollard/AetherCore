#pragma once

#include <algorithm>
#include <vector>

#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::app
{
	// Shared editor selection state (multi-select + a "primary" for the inspector).
	// Registered in the ServiceContainer by DebugLayer; read by the outliner, the
	// inspector, and (Spec 2) viewport picking.
	class SceneSelection
	{
	public:
		void Select(Entity e)
		{
			m_selected.clear();
			if (e.IsValid())
			{
				m_selected.push_back(e);
			}
			m_primary = e;
			m_lastChangeSerial++;
		}

		void AddToSelection(Entity e)
		{
			if (e.IsValid() && !Contains(e))
			{
				m_selected.push_back(e);
			}
			m_primary = e;
			m_lastChangeSerial++;
		}

		void ToggleSelection(Entity e)
		{
			if (!e.IsValid())
			{
				return;
			}
			const auto it = std::find(m_selected.begin(), m_selected.end(), e);
			if (it != m_selected.end())
			{
				m_selected.erase(it);
				m_primary = m_selected.empty() ? Entity{} : m_selected.back();
			}
			else
			{
				m_selected.push_back(e);
				m_primary = e;
			}
			m_lastChangeSerial++;
		}

		void Clear()
		{
			if (!m_selected.empty() || m_primary.IsValid())
			{
				m_lastChangeSerial++;
			}
			m_selected.clear();
			m_primary = {};
		}

		[[nodiscard]] bool Contains(Entity e) const
		{
			return std::find(m_selected.begin(), m_selected.end(), e) != m_selected.end();
		}

		[[nodiscard]] Entity Primary() const
		{
			return m_primary;
		}

		[[nodiscard]] const std::vector<Entity>& All() const
		{
			return m_selected;
		}

		// Bumps on every selection mutation; panels compare it to drive
		// selection-change animations without polling entity lists.
		[[nodiscard]] std::uint64_t ChangeSerial() const
		{
			return m_lastChangeSerial;
		}

		// Drops entities that are no longer alive (call once per frame).
		void Prune(const World& world)
		{
			const auto dead = [&](Entity e) { return !e.IsValid() || !world.GetRegistry().valid(World::ToEntt(e)); };
			const auto before = m_selected.size();
			std::erase_if(m_selected, dead);
			if (m_selected.size() != before)
			{
				m_lastChangeSerial++;
			}
			if (dead(m_primary))
			{
				m_primary = m_selected.empty() ? Entity{} : m_selected.back();
			}
		}

	private:
		std::vector<Entity> m_selected;
		Entity m_primary{};
		std::uint64_t m_lastChangeSerial = 0;
	};
} // namespace aether::app
