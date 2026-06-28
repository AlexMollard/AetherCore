#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AppLayer.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/Renderer.hpp"
#include "scene/Entity.hpp"
#include "utils/TomlConfig.hpp"

namespace aether
{
	class RenderGraph;
	class World;
} // namespace aether

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
		static constexpr std::size_t kRenderBenchmarkSampleCount = 240;

		struct RenderPassBenchmark
		{
			std::array<float, kRenderBenchmarkSampleCount> samples{};
			std::size_t head = 0;
			std::size_t count = 0;
		};

		static const char* GetTonemapModeName(aether::TonemapMode mode);

		void PollScriptErrors(LayerContext& context);
		void PushFrameSample(float frameMs);
		void LoadSettings(LayerContext& context);
		void SaveSettings(LayerContext& context);
		void DrawSceneViewport(LayerContext& context);
		void DrawTextureInspector(LayerContext& context);
		void DrawRenderGraphDebugger(LayerContext& context, RenderGraph& graph);
		void ReleaseSceneViewportTexture(LayerContext& context);
		void ReleaseTextureInspectorTextures(LayerContext& context);

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

		TomlConfig m_debugConfig;
		std::deque<ScriptErrorToast> m_errorToasts;
		Entity m_selectedSceneEntity;
		std::uint64_t m_sceneViewportTextureId = 0;
		gpu::ImageView m_sceneViewportImageView = nullptr;
		bool m_dockspaceBuilt = false;

		int m_viewportDisplayMode = 0; // Fit, fill, actual, integer
		int m_viewportAspectMode = 0;  // Render, free, 16:9, 16:10, 4:3, 1:1
		bool m_viewportShowStats = true;
		bool m_viewportShowMouse = true;

		std::unordered_map<std::string, RenderPassBenchmark> m_renderPassBenchmarks;
		std::string m_selectedRenderPass;
		char m_renderGraphFilter[96] = {};
		bool m_renderGraphShowDisabled = true;
		bool m_renderGraphShowCulled = true;
		bool m_renderGraphAutoSelectHotPass = false;

		std::unordered_map<std::uint32_t, std::uint64_t> m_textureInspectorTextureIds;
		std::uint32_t m_selectedTextureBits = 0;
		int m_texturePreviewChannel = 0;
		float m_texturePreviewZoom = 1.0f;
		bool m_texturePreviewCheckerboard = true;
	};
} // namespace aether::app
