#pragma once

#include <string>
#include <string_view>

#include "debug/DebugPanel.hpp"
#include "materialgraph/MaterialGraph.hpp"

namespace aether::editor
{
	// Node graph authoring for a material's shader.
	//
	// Compiling writes a .slang beside the project's other shaders and runs it through the
	// same compiler the project uses, so a generated material is an ordinary project shader
	// afterwards - nothing downstream knows or cares that a graph produced it.
	class MaterialGraphPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Material Graph";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		void DrawToolbar(app::LayerContext& context);
		void DrawCanvas();
		void SyncLinks();
		void Compile(app::LayerContext& context);

		MaterialGraph m_graph = MakeDefaultMaterialGraph();
		std::string m_path;      // the .materialgraph.toml being edited
		std::string m_status;
		bool m_statusIsError = false;
		bool m_positionsApplied = false;
	};
} // namespace aether::editor
