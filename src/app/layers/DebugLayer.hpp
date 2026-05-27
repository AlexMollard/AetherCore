#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <string>

#include "AppLayer.hpp"
#include "rendering/Renderer.hpp"

namespace aether::app::scripting
{
	class ScriptingSubsystem;
}

namespace aether::app
{
	// Non-blocking toast notification for script errors.
	// Always visible regardless of m_visible (the debug panel toggle).
	struct ScriptErrorToast
	{
		std::string message;
		std::string summary;
		std::string filePath;
		int line = 0;
		bool dismissed = false;
		bool detailsExpanded = false;
		bool copied = false;
		double copyFeedbackTime = 0.0;
	};

	class DebugLayer final : public AppLayer
	{
	public:
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		static constexpr std::size_t kFrameHistorySize = 128;
		static constexpr double kToastAutoDismissSeconds = 10.0;

		static const char* GetTonemapModeName(aether::TonemapMode mode);

		float GetAverageFrameTimeMs() const;
		float GetMinFrameTimeMs() const;
		float GetMaxFrameTimeMs() const;

		void DrawFrameTimeGraph() const;

		void PollScriptErrors(LayerContext& context);
		void DrawErrorToasts(LayerContext& context);

		static void ParseErrorLocation(const std::string& error, std::string& outPath, int& outLine);
		static void OpenInVSCode(const std::string& filePath, int line);

		std::array<float, kFrameHistorySize> m_frameTimesMs{};
		std::size_t m_frameHistoryHead = 0;
		std::size_t m_frameHistoryCount = 0;
		bool m_visible = true;
		std::deque<ScriptErrorToast> m_errorToasts;
	};
} // namespace aether::app
