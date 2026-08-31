#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"
#include "material/MaterialSerializer.hpp"
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

		// The material asset currently open for editing. Held here rather than re-read every
		// frame so a slider drag edits one in-memory copy, and written back only once the
		// drag ends - see DrawMaterialAssetEditor.
		std::string m_materialAssetPath;
		MaterialPresetSpec m_materialAsset;
		bool m_materialAssetLoaded = false;
		bool m_materialAssetDirty = false;
		std::string m_materialAssetError;
	};
} // namespace aether::editor
