#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"
#include "debug/MaterialAssetEditor.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::editor
{
	class InspectorPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Inspector";
		}

		// Part of the default layout: what the selected thing is.
		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		static bool IsAlive(const World& world, Entity entity);

		char m_addFilter[48] = {};
		bool m_addFocusPending = false;
		char m_addTagBuf[48] = {};

		// The material asset currently open for editing, if the selection is one.
		MaterialAssetEditState m_materialEdit;
	};
} // namespace aether::editor
