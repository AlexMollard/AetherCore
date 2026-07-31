#pragma once

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "debug/DebugPanel.hpp"
#include "editor/EditorProjectActions.hpp"

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	// Everything that turns the open project into something runnable: pack, shaders, engine
	// pak, the C# debugger, and publish. Split out of ProjectPanel, which is about what the
	// project IS rather than what you build from it.
	class BuildPanel final : public DebugPanel
	{
	public:
		~BuildPanel() override;

		std::string_view GetName() const override
		{
			return "Build";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		struct PublishTask
		{
			std::atomic<float> completion = 0.0f;
			std::mutex mutex;
			std::string stage;
		};

		// One status for the whole panel. Four parallel status/succeeded pairs was the old
		// shape, and it meant a stale pack message sat under a fresh publish failure.
		struct ActionStatus
		{
			std::string message;
			std::string remediation;
			bool ok = false;
			std::filesystem::path output;
		};

		void DrawConfigBanner(const app::EditorProjectContext& project);
		void DrawActionRow(app::LayerContext& context, const app::EditorProjectContext& project);
		void DrawScriptDebuggerRow(app::LayerContext& context);
		void DrawPublishProgress();
		void DrawStatus();
		void PollPublish();
		void SetStatus(const EditorProjectActionResult& result);

		ActionStatus m_status;
		std::filesystem::path m_visualStudioInstall;
		std::filesystem::path m_lastPublishPath;
		bool m_lastPublishSucceeded = false;
		std::future<EditorProjectActionResult> m_publishFuture;
		std::shared_ptr<PublishTask> m_publishTask;
	};
} // namespace aether::editor
