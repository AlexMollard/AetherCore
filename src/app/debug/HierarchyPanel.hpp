#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
} // namespace aether

namespace aether::app
{
	class SceneSelection;

	// The scene outliner. Draws the window titled "Scene" (the saved dock-node
	// mapping keys off the window title, so the title stays even though the
	// panel class replaced InspectorPanel's old flat list).
	class HierarchyPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Scene Outliner";
		}

		void OnImGui(LayerContext& context) override;

	private:
		void DrawNode(World& world, SceneSelection& selection, Entity e);
		void DrawRowBackdrop(const SceneSelection& selection, Entity e);
		void DrawRowContent(World& world, Entity e);
		void HandleRowClick(SceneSelection& selection, Entity e);
		// Returns true if the menu destroyed `e` (callers must not touch it after).
		bool DrawRowContextMenu(World& world, SceneSelection& selection, Entity e);
		void BeginRename(const World& world, Entity e);

		char m_search[64] = {};
		// Kind filter chips (OR-combined; none active == show everything).
		bool m_filterMesh = false;
		bool m_filterSkinned = false;
		bool m_filterPhysics = false;
		bool m_filterEffect = false;

		// Inline rename state.
		Entity m_renaming{};
		char m_renameBuf[64] = {};
		bool m_renameFocusPending = false;

		// Shift-range selection works over the rows as the user sees them; the
		// previous frame's order is complete by the time a click arrives.
		std::vector<Entity> m_rowsCur;
		std::vector<Entity> m_rowsPrev;
		Entity m_rangeAnchor{};

		// Drag-drop reparent is queued during the tree walk and applied after it:
		// SetParent mutates children vectors the recursion may still be iterating.
		std::optional<std::pair<Entity, Entity>> m_pendingReparent; // {child, newParent}

		// Juice: newly-seen entities flash briefly; selection changes pulse.
		std::unordered_set<std::uint32_t> m_knownIds;
		std::unordered_map<std::uint32_t, double> m_spawnFlash; // id -> first-seen time
		bool m_knownSeeded = false;                             // no flash on the initial population
		std::uint64_t m_seenSelectionSerial = 0;
		double m_pulseStart = -1.0;
	};
} // namespace aether::app
