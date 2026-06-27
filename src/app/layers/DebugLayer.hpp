#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "AppLayer.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

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
		static constexpr std::size_t kMaxRenderPassRows = 16;
		static constexpr std::size_t kMaxSceneRows = 80;
		static constexpr std::size_t kFrameSampleCount = 180;

		static const char* GetTonemapModeName(aether::TonemapMode mode);

		void PollScriptErrors(LayerContext& context);
		void PushFrameSample(float frameMs);

		static void ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine);
		static void OpenInVSCode(const std::string& filePath, int line);

		bool m_visible = true;
		bool m_debugTestShapes = true; // F7: show diagnostic test shapes
		bool m_lightGizmos = true;     // F9: show light volume/direction gizmos
		bool m_lightGizmoPointVolumes = true;
		bool m_lightGizmoSpotCones = true;
		bool m_lightGizmoSunDirection = true;
		bool m_lightGizmoShadowMarkers = true;
		float m_lightGizmoScale = 1.0f;

		std::array<float, kFrameSampleCount> m_frameSamples{};
		std::size_t m_frameSampleHead = 0;
		std::size_t m_frameSampleCount = 0;

		std::deque<ScriptErrorToast> m_errorToasts;
		Entity m_selectedSceneEntity;
		std::unordered_set<std::uint32_t> m_expandedSceneEntities;
	};
} // namespace aether::app
