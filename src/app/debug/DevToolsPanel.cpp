#include "DevToolsPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <imgui.h>

#include "PlayState.hpp"
#include "debug/DebugPanel.hpp"
#include "layers/AppLayer.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "rendering/RenderQueue.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::editor
{
	void DevToolsPanel::OnUpdate(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		// Resolved every frame rather than on the play transition, so it is correct however
		// play mode is entered - the toolbar button, a keybind, or the control server - and
		// there is no transition hook to forget to hook up.
		const auto* play = context.TryGet<app::PlayState>();
		const bool suppressForPlay = play != nullptr && play->IsPlaying() && !aether::AreEditorGizmosInPlayEnabled();
		aether::SetPhysicsDebugShapesEnabled(m_physicsShapesWanted && !suppressForPlay);
	}

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

			// Bound to the wish, not to the live flag: while playing the live flag reads
			// false, and binding to it would make the checkbox appear to untick itself.
			if (ImGui::Checkbox("Physics debug rendering", &m_physicsShapesWanted))
			{
				aether::SetPhysicsDebugShapesEnabled(m_physicsShapesWanted);
			}

			bool gizmosInPlay = aether::AreEditorGizmosInPlayEnabled();
			if (ImGui::Checkbox("Editor gizmos while playing", &gizmosInPlay))
			{
				aether::SetEditorGizmosInPlayEnabled(gizmosInPlay);
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("Keep light volumes and physics shapes on screen during play. Off by default so Play shows the game itself.");
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
		m_physicsShapesWanted = config.GetBool("debug.physicsdebugrendering", aether::IsPhysicsDebugShapesEnabled());
		aether::SetPhysicsDebugShapesEnabled(m_physicsShapesWanted);
		aether::SetEditorGizmosInPlayEnabled(config.GetBool("debug.editorgizmosinplay", aether::AreEditorGizmosInPlayEnabled()));
	}

	void DevToolsPanel::SaveSettings(TomlConfig& config, app::LayerContext& context) const
	{
		(void) context;
		config.Set("debug.debugoverlay", aether::IsDebugRenderingEnabled());
		// The wish, not the live flag - see the member's declaration.
		config.Set("debug.physicsdebugrendering", m_physicsShapesWanted);
		config.Set("debug.editorgizmosinplay", aether::AreEditorGizmosInPlayEnabled());
	}
} // namespace aether::editor
