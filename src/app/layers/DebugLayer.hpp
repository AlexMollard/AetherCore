#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include "AppLayer.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Entity.hpp"
#include "ui/UiLayout.hpp"

namespace aether::app::scripting
{
	class ScriptingSubsystem;
}

namespace aether::app
{
	// Non-blocking error notification for script errors.
	// Always visible regardless of m_visible (the debug panel toggle).
	struct ScriptErrorToast
	{
		std::string message;
		std::string summary;
		std::string filePath;
		int line = 0;
		bool dismissed = false;
	};

	class DebugLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		enum LabelRow : std::size_t
		{
			Row_Frame,
			Row_Fps,
			Row_Delta,
			Row_AvgFps,
			Row_Min,
			Row_Max,
			Row_Tonemap,
			Row_Fxaa,
			Row_Resolution,
			Row_Pos,
			Row_Fov,
			Row_Near,
			Row_Far,
			Row_PointLights,
			Row_SpotLights,
			Row_SunIntensity,
			Row_PhysicsDebug,
			Row_ForwardRender,
			Row_FirstRenderPass,
		};

		static constexpr std::size_t kMaxRenderPassRows = 16;
		static constexpr std::size_t kLabelRowCount = Row_FirstRenderPass + kMaxRenderPassRows;

		enum Tab : std::size_t
		{
			Tab_Performance,
			Tab_Render,
			Tab_Camera,
			kTabCount
		};

		static const char* GetTonemapModeName(aether::TonemapMode mode);

		void PollScriptErrors(LayerContext& context);

		static void ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine);
		static void OpenInVSCode(const std::string& filePath, int line);

		bool m_visible = true;
		bool m_debugTestShapes = true; // F7: show diagnostic test shapes
		aether::UiRect m_savedPanelRect{};
		std::deque<ScriptErrorToast> m_errorToasts;

		Entity m_debugPanel;
		Entity m_headerSpacer;
		Entity m_tabBar;
		Entity m_graphEntity;
		Entity m_labelRows[kLabelRowCount];
		Entity m_separators[6];
		Entity m_reloadButton;
		Entity m_passTotalRow;
		std::array<Entity, kMaxRenderPassRows> m_passBars{};
		std::vector<Entity> m_entities;
		std::array<Entity, kTabCount> m_tabPages;
	};
} // namespace aether::app
