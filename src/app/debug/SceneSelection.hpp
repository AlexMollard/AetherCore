#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	// Shared editor selection state (multi-select + a "primary" for the inspector).
	// Registered in the ServiceContainer by DebugLayer; read by the outliner, the
	// inspector, and (Spec 2) viewport picking.
	class SceneSelection
	{
	public:
		enum class AssetKind
		{
			None,
			Model,
			Material,
			Texture,
			Script,
			Prefab,
			Scene,
			File,
		};

		struct Asset
		{
			AssetKind kind = AssetKind::None;
			std::string path;
			std::string displayName;
		};

		void Select(Entity e)
		{
			m_selected.clear();
			if (e.IsValid())
			{
				m_selected.push_back(e);
			}
			m_primary = e;
			if (m_primary.IsValid())
			{
				m_lastEntityPrimary = m_primary;
			}
			m_asset = {};
			m_lastChangeSerial++;
		}

		void AddToSelection(Entity e)
		{
			if (e.IsValid() && !Contains(e))
			{
				m_selected.push_back(e);
			}
			m_primary = e;
			if (m_primary.IsValid())
			{
				m_lastEntityPrimary = m_primary;
			}
			m_asset = {};
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
			if (m_primary.IsValid())
			{
				m_lastEntityPrimary = m_primary;
			}
			m_asset = {};
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
			m_asset = {};
		}

		void Replace(std::vector<Entity> selected, Entity primary)
		{
			std::erase_if(selected, [](Entity e) { return !e.IsValid(); });
			for (auto it = selected.begin(); it != selected.end(); ++it)
			{
				selected.erase(std::remove(it + 1, selected.end(), *it), selected.end());
			}
			if (!primary.IsValid() || std::find(selected.begin(), selected.end(), primary) == selected.end())
			{
				primary = selected.empty() ? Entity{} : selected.back();
			}
			if (m_selected != selected || m_primary != primary || m_asset.kind != AssetKind::None)
			{
				m_selected = std::move(selected);
				m_primary = primary;
				if (m_primary.IsValid())
				{
					m_lastEntityPrimary = m_primary;
				}
				m_asset = {};
				m_lastChangeSerial++;
			}
		}

		void SelectAsset(AssetKind kind, std::string path, std::string displayName = {})
		{
			if (displayName.empty())
			{
				displayName = path;
			}
			Asset next{kind, std::move(path), std::move(displayName)};
			if (!m_selected.empty() || m_primary.IsValid() || m_asset.kind != next.kind || m_asset.path != next.path || m_asset.displayName != next.displayName)
			{
				m_selected.clear();
				m_primary = {};
				m_asset = std::move(next);
				m_lastChangeSerial++;
			}
		}

		[[nodiscard]] bool HasAsset() const
		{
			return m_asset.kind != AssetKind::None && !m_asset.path.empty();
		}

		[[nodiscard]] const Asset& SelectedAsset() const
		{
			return m_asset;
		}

		[[nodiscard]] Entity LastEntityPrimary() const
		{
			return m_lastEntityPrimary;
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
			const auto dead = [&](Entity e)
			{
				return !e.IsValid() || !world.GetRegistry().valid(World::ToEntt(e));
			};
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
			if (dead(m_lastEntityPrimary))
			{
				m_lastEntityPrimary = {};
			}
		}

	private:
		std::vector<Entity> m_selected;
		Entity m_primary{};
		Entity m_lastEntityPrimary{};
		Asset m_asset{};
		std::uint64_t m_lastChangeSerial = 0;
	};
} // namespace aether::editor
