#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
} // namespace aether

namespace aether::app
{
	// The entity editor: header (name/id/kind), add-component palette, and one
	// collapsing section per present component (see debug/ComponentDrawers.hpp).
	class InspectorPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Inspector";
		}

		void OnImGui(LayerContext& context) override;

	private:
		static bool IsAlive(const World& world, Entity entity);

		char m_addFilter[48] = {};
		bool m_addFocusPending = false;
		char m_addTagBuf[48] = {};
	};
} // namespace aether::app
