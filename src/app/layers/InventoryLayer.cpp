#include "InventoryLayer.hpp"

#include <format>
#include <glm/glm.hpp>

#include "scene/AetherCore.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiTheme.hpp"
#include "ui/UiWidgets.hpp"
#include "ui/UiWorld.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
	// ── GW2-style rarity colours ──────────────────────────────────────────────
	static constexpr glm::vec4 kRarityJunk{ 0.60f, 0.60f, 0.60f, 1.f };
	static constexpr glm::vec4 kRarityFine{ 0.27f, 0.45f, 0.83f, 1.f };
	static constexpr glm::vec4 kRarityMasterwork{ 0.15f, 0.55f, 0.20f, 1.f };
	static constexpr glm::vec4 kRarityRare{ 0.80f, 0.72f, 0.14f, 1.f };
	static constexpr glm::vec4 kRarityExotic{ 0.85f, 0.49f, 0.09f, 1.f };
	static constexpr glm::vec4 kRarityAscended{ 0.71f, 0.13f, 0.53f, 1.f };
	static constexpr glm::vec4 kRarityLegendary{ 0.27f, 0.85f, 0.78f, 1.f };

	struct FakeItem
	{
		glm::vec4 rarity;
		int quantity;
	};

	// 48 fake items - some slots intentionally empty (quantity 0).
	static constexpr FakeItem kItems[kInventorySlots] = {
		// Row 1 — assorted loot
		{  kRarityLegendary,   1 },
		{   kRarityAscended,   1 },
		{     kRarityExotic,   1 },
		{       kRarityRare,   3 },
		{ kRarityMasterwork,   5 },
		{       kRarityFine,  12 },
		{       kRarityJunk,  50 },
		{		        {},   0 }, // empty

		// Row 2
		{     kRarityExotic,   1 },
		{       kRarityRare,   2 },
		{ kRarityMasterwork,   8 },
		{       kRarityFine,  25 },
		{       kRarityFine,  17 },
		{       kRarityJunk, 100 },
		{		        {},   0 },
		{		        {},   0 },

		// Row 3
		{   kRarityAscended,   1 },
		{       kRarityRare,   1 },
		{       kRarityRare,   1 },
		{ kRarityMasterwork,   3 },
		{ kRarityMasterwork,   3 },
		{       kRarityFine,  42 },
		{       kRarityJunk,  99 },
		{		        {},   0 },

		// Row 4
		{     kRarityExotic,   1 },
		{     kRarityExotic,   1 },
		{       kRarityRare,   4 },
		{       kRarityFine,  30 },
		{       kRarityFine,   7 },
		{       kRarityJunk,  18 },
		{		        {},   0 },
		{		        {},   0 },

		// Row 5
		{  kRarityLegendary,   1 },
		{   kRarityAscended,   1 },
		{ kRarityMasterwork,   2 },
		{ kRarityMasterwork,   6 },
		{       kRarityFine,  11 },
		{       kRarityJunk,  33 },
		{		        {},   0 },
		{		        {},   0 },

		// Row 6 — mostly empty (end of bag)
		{       kRarityRare,   1 },
		{       kRarityFine,   9 },
		{		        {},   0 },
		{		        {},   0 },
		{		        {},   0 },
		{		        {},   0 },
		{		        {},   0 },
		{		        {},   0 },
	};

	// Panel and grid dimensions (must be consistent with constants above).
	static constexpr float kSlotSize = 50.f;
	static constexpr float kSlotSpacing = 4.f;
	static constexpr float kGridPad = 8.f;
	static constexpr float kHeaderH = 48.f;
	// Grid pixel dimensions (computed from the grid description).
	static constexpr float kGridW = static_cast<float>(kInventoryCols) * kSlotSize + static_cast<float>(kInventoryCols - 1) * kSlotSpacing;
	static constexpr float kGridH = static_cast<float>(kInventoryRows) * kSlotSize + static_cast<float>(kInventoryRows - 1) * kSlotSpacing;
	static constexpr float kPanelW = kGridW + 2.f * kGridPad;
	static constexpr float kPanelH = kHeaderH + kGridPad + kGridH + kGridPad;

	// ── OnAttach ─────────────────────────────────────────────────────────────

	void InventoryLayer::OnAttach(LayerContext& context)
	{
		auto& world = context.Get<ui::UiWorld>();

		auto reg = [this](Entity e) -> Entity
		{
			m_entities.push_back(e);
			return e;
		};

		// Panel: centred, draggable, collapsible.
		// UiChildrenComponent is added so BringToFront can raise the entire
		// panel subtree (grid container + all slots) by the same delta, keeping
		// slots above the panel body at all times.
		m_panel = reg(ui::SpawnPanel(world,
		        UiAnchors::Center({ kPanelW, kPanelH }),
		        std::format("Inventory  \xC2\xB7  Bag 1  ({}/{})", 40, kInventorySlots),
		        /*draggable=*/true,
		        /*collapsible=*/true,
		        /*zOrder=*/1.f));
		world.Emplace<ui::UiChildrenComponent>(m_panel);

		// Grid container: initially a zero-size rect; repositioned in OnGui to
		// sit below the panel header. The rect is overwritten every frame, so the
		// initial value doesn't matter.
		// SpawnItemGrid creates the container WITHOUT UiParentComponent; AddChild
		// wires both the parent's children list and the child's UiParentComponent.
		m_gridContainer = reg(ui::SpawnItemGrid(world,
		        /*containerRect=*/{},
		        kInventoryCols,
		        kSlotSize,
		        kSlotSpacing,
		        kGridPad,
		        kInventorySlots,
		        /*slotZOrder=*/2.f,
		        m_slots.data(),
		        /*containerZOrder=*/1.5f));
		ui::AddChild(world, m_panel, m_gridContainer);

		// Register slots for cleanup (SpawnItemGrid already created them but we
		// need them in m_entities for OnDetach to destroy them).
		for (const Entity slot: m_slots)
		{
			m_entities.push_back(slot);
		}

		// Populate slots with fake item data.
		for (int i = 0; i < kInventorySlots; ++i)
		{
			if (auto* s = world.TryGet<ui::UiItemSlotComponent>(m_slots[i]))
			{
				s->quantity = kItems[i].quantity;
				s->rarityColor = kItems[i].rarity;
			}
		}

		INFO(LogCategory::App, "InventoryLayer attached ({} entities).", m_entities.size());
	}

	// ── OnDetach ─────────────────────────────────────────────────────────────

	void InventoryLayer::OnDetach(LayerContext& context)
	{
		auto& world = context.Get<ui::UiWorld>();
		for (const Entity e: m_entities)
		{
			world.Destroy(e);
		}
		m_entities.clear();
		INFO(LogCategory::App, "InventoryLayer detached.");
	}

	// ── OnGui ─────────────────────────────────────────────────────────────────

	void InventoryLayer::OnGui(LayerContext& context)
	{
		auto& world = context.Get<ui::UiWorld>();
		UIRenderer& ui = context.Get<UIRenderer>();
		const VkExtent2D extent = context.Get<Swapchain>().GetExtent();
		const ui::UiTheme& theme = ui::UiTheme::Default();

		// ── Anchor grid container below the panel header ───────────────────────
		// The panel can be dragged, so we re-derive the grid position from the
		// panel's current resolved pixel rect each frame.
		if (const auto* pt = world.TryGet<ui::UiTransformComponent>(m_panel))
		{
			const glm::vec4 panelPx = ResolveUiRectPx(extent, pt->rect);
			const float gridX = panelPx.x + kGridPad;
			const float gridY = panelPx.y + kHeaderH + kGridPad;

			if (auto* gt = world.TryGet<ui::UiTransformComponent>(m_gridContainer))
			{
				// Use anchor {0,0} so offsetMinPx/offsetMaxPx are plain pixel coords.
				gt->rect = UiRect{
					.anchorMin = {            0.f,            0.f },
					.anchorMax = {            0.f,            0.f },
					.offsetMinPx = {          gridX,          gridY },
					.offsetMaxPx = { gridX + kGridW, gridY + kGridH },
				};
			}
		}

		// ── Layout pass ───────────────────────────────────────────────────────
		// ApplyGridLayout positions each slot inside the grid container.
		ui::RunLayouts(world, extent);

		// ── Draw ─────────────────────────────────────────────────────────────
		if (ui::DrawPanel(world, m_panel, ui, extent, theme))
		{
			for (int i = 0; i < kInventorySlots; ++i)
			{
				if (ui::DrawItemSlot(world, m_slots[i], ui, extent, theme))
				{
					m_selectedSlot = (m_selectedSlot == i) ? -1 : i;
				}

				// Sync selected state to component so DrawItemSlot can read it.
				if (auto* s = world.TryGet<ui::UiItemSlotComponent>(m_slots[i]))
				{
					s->selected = (m_selectedSlot == i);
				}
			}
		}
	}

} // namespace aether::app
