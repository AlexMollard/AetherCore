#include "DevToolsPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <imgui.h>

#include "debug/DebugPanel.hpp"
#include "layers/AppLayer.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "rendering/RenderQueue.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	void DevToolsPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Debug", VisiblePtr());
		chrome::PanelHeader("DEV TOOLS");
		{
			bool debugRenderer = aether::IsDebugRenderingEnabled();
			if (ImGui::Checkbox("Debug overlay", &debugRenderer))
			{
				aether::SetDebugRenderingEnabled(debugRenderer);
			}

			bool physicsShapes = aether::IsPhysicsDebugShapesEnabled();
			if (ImGui::Checkbox("Physics debug rendering", &physicsShapes))
			{
				aether::SetPhysicsDebugShapesEnabled(physicsShapes);
			}

			ImGui::SeparatorText("Render");
			RenderQueue& renderQueue = context.Get<RenderQueue>();
			bool forceVisible = renderQueue.IsDebugForceVisible();
			if (ImGui::Checkbox("Force visible", &forceVisible))
			{
				renderQueue.SetDebugForceVisible(forceVisible);
			}
			bool bypassIndirect = renderQueue.IsDebugBypassIndirect();
			if (ImGui::Checkbox("Bypass indirect", &bypassIndirect))
			{
				renderQueue.SetDebugBypassIndirect(bypassIndirect);
			}
			bool disableAnimation = renderQueue.IsDebugDisableAnimation();
			if (ImGui::Checkbox("Disable animation", &disableAnimation))
			{
				renderQueue.SetDebugDisableAnimation(disableAnimation);
			}

			if (ImGui::Button("Reload Scripts"))
			{
				if (auto* scripting = context.TryGet<app::scripting::CSharpScriptingSubsystem>())
				{
					scripting->RequestReload();
				}
			}
		}
		ImGui::End();
	}

	void DevToolsPanel::LoadSettings(TomlConfig& config, app::LayerContext& context)
	{
		(void) context;
		const bool overlay = config.GetBool("debug.debugoverlay", aether::IsDebugRenderingEnabled());
		aether::SetDebugRenderingEnabled(overlay);
		const bool physicsShapes = config.GetBool("debug.physicsdebugrendering", aether::IsPhysicsDebugShapesEnabled());
		aether::SetPhysicsDebugShapesEnabled(physicsShapes);
	}

	void DevToolsPanel::SaveSettings(TomlConfig& config, app::LayerContext& context) const
	{
		(void) context;
		config.Set("debug.debugoverlay", aether::IsDebugRenderingEnabled());
		config.Set("debug.physicsdebugrendering", aether::IsPhysicsDebugShapesEnabled());
	}
} // namespace aether::editor
