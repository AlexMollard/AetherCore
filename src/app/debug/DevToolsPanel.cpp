#include "DevToolsPanel.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "Color.hpp"
#include "debug/DebugPanel.hpp"
#include "layers/AppLayer.hpp"
#include "physics/PhysicsDebugRenderer.hpp"
#include "platform/Input.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::app
{
	void DevToolsPanel::OnUpdate(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		// Debug-overlay and test-shapes toggles live on this panel's checkboxes now;
		// the editor keeps only scene-manipulation keybinds.
		if (m_debugTestShapes)
		{
			if (auto engine = context.TryGet<aether::AetherCore>())
			{
				auto& verts = engine->GetPendingDebugVertices();
				if (const aether::Camera* cam = context.Get<CameraManager>().TryGetMainCamera())
				{
					const glm::vec3 boxCenter = cam->GetPosition() + cam->GetForward() * 2.0f;
					AddDebugAabb(verts, boxCenter - glm::vec3(0.5f), boxCenter + glm::vec3(0.5f), colors::Red);
					AddDebugAxes(verts, glm::translate(glm::mat4(1.0f), boxCenter), 0.75f);
				}

				AddDebugAabb(verts, glm::vec3(-2.5f), glm::vec3(2.5f), colors::Yellow);
				AddDebugSphere(verts, glm::vec3(0.0f), 2.0f, colors::Info, 16);
				AddDebugLine(verts, glm::vec3(0.0f, -5.0f, 0.0f), glm::vec3(0.0f, 5.0f, 0.0f), colors::Neutral);
			}
		}
	}

	void DevToolsPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Debug", VisiblePtr());
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

			auto& physicsDebug = context.Get<aether::RenderingSubsystem>().GetPhysicsDebugRenderer();
			bool selfTest = physicsDebug.IsSelfTestEnabled();
			if (ImGui::Checkbox("Physics renderer self-test", &selfTest))
			{
				physicsDebug.SetSelfTestEnabled(selfTest);
			}

			ImGui::Checkbox("Test shapes", &m_debugTestShapes);

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
				if (auto scripting = context.TryGet<scripting::CSharpScriptingSubsystem>())
				{
					scripting->RequestReload();
				}
			}
		}
		ImGui::End();
	}

	void DevToolsPanel::LoadSettings(TomlConfig& config, LayerContext& context)
	{
		(void) context;
		m_debugTestShapes = config.GetBool("debug.testshapes", m_debugTestShapes);
		bool overlay = config.GetBool("debug.debugoverlay", aether::IsDebugRenderingEnabled());
		aether::SetDebugRenderingEnabled(overlay);
		bool physicsShapes = config.GetBool("debug.physicsdebugrendering", aether::IsPhysicsDebugShapesEnabled());
		aether::SetPhysicsDebugShapesEnabled(physicsShapes);
		auto& physicsDebug = context.Get<aether::RenderingSubsystem>().GetPhysicsDebugRenderer();
		bool selfTest = config.GetBool("debug.physicsdebugselftest", physicsDebug.IsSelfTestEnabled());
		physicsDebug.SetSelfTestEnabled(selfTest);
	}

	void DevToolsPanel::SaveSettings(TomlConfig& config, LayerContext& context) const
	{
		config.Set("debug.testshapes", m_debugTestShapes);
		config.Set("debug.debugoverlay", aether::IsDebugRenderingEnabled());
		config.Set("debug.physicsdebugrendering", aether::IsPhysicsDebugShapesEnabled());
		auto& physicsDebug = context.Get<aether::RenderingSubsystem>().GetPhysicsDebugRenderer();
		config.Set("debug.physicsdebugselftest", physicsDebug.IsSelfTestEnabled());
	}
} // namespace aether::app
