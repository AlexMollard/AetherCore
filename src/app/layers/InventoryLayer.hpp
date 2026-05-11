#pragma once

#include <array>
#include <vector>

#include "AppLayer.hpp"
#include "scene/Entity.hpp"

namespace aether::app
{
	static constexpr int kInventoryCols = 8;
	static constexpr int kInventoryRows = 6;
	static constexpr int kInventorySlots = kInventoryCols * kInventoryRows;

	class InventoryLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		Entity m_panel;
		Entity m_gridContainer;
		std::array<Entity, kInventorySlots> m_slots{};

		int m_selectedSlot = -1;

		std::vector<Entity> m_entities;
	};
} // namespace aether::app
