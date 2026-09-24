#include "AetherCore.hpp"
#include "debug/BuildPanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/InspectorWidgets.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <imgui.h>

#ifdef _WIN32
#	include <Windows.h>
#	include <shellapi.h>
#endif

#include "Icons.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/EditorProjectPublisher.hpp"
#include "layers/AppLayer.hpp"
#include "utils/Profiler.hpp"

namespace aether::editor
{
	namespace
	{
		std::string DisplayPath(const std::filesystem::path& path)
		{
			return path.empty() ? std::string{} : path.lexically_normal().string();
		}

		bool FolderExists(const std::filesystem::path& path)
		{
			std::error_code ec;
			return std::filesystem::is_directory(path, ec);
		}

		void OpenFolderInShell(const std::filesystem::path& path)
		{
#ifdef _WIN32
			if (path.empty())
			{
				return;
			}
			ShellExecuteW(nullptr, L"open", path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			static_cast<void>(path);
#endif
		}

		// Launch the published game detached, with its working directory set to the build
		// folder so the runtime resolves data/ (engine.pak, project.pak, ...).
		void LaunchGameBuild(const std::filesystem::path& packageDir, const std::string& exeName)
		{
#ifdef _WIN32
			if (packageDir.empty())
			{
				return;
			}
			const std::filesystem::path exe = packageDir / exeName;
			ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr, packageDir.wstring().c_str(), SW_SHOWNORMAL);
#else
			static_cast<void>(packageDir);
			static_cast<void>(exeName);
#endif
		}

		struct ButtonRow
		{
			float rightEdge = 0.0f;
			bool started = false;

			ButtonRow()
			{
				rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
			}

			void Item(const char* label)
			{
				const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
				if (started)
				{
					const float nextX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
					if (nextX + width <= rightEdge)
					{
						ImGui::SameLine();
					}
				}
				started = true;
			}
		};
	} // namespace

	BuildPanel::~BuildPanel()
	{
		// A future from std::async blocks in its destructor until the task finishes, so
		// tearing the editor down mid-publish would stall shutdown until the pack + shader +
		// dotnet build completed. The task is self-contained, so hand an in-flight future to
		// a detached waiter instead of blocking teardown on it.
		if (m_publishFuture.valid())
		{
			std::thread([f = std::move(m_publishFuture)]() mutable { f.wait(); }).detach();
		}
	}

	void BuildPanel::SetStatus(const EditorProjectActionResult& result)
	{
		m_status.ok = result.succeeded;
		m_status.message = result.message;
		m_status.remediation = result.remediation;
		m_status.output = result.outputPath;
	}

	void BuildPanel::DrawConfigBanner(const app::EditorProjectContext& project)
	{
		const PublishPlan plan = PlanPublish(project);
		if (!plan.shippableConfig)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(chrome::kWarning, "%s build - for testing, not for shipping.", plan.configName.c_str());
			ImGui::PopTextWrapPos();
		}
		else
		{
			ImGui::TextColored(chrome::kSuccess, "%s build.", plan.configName.c_str());
		}
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("Publishes to %s", DisplayPath(plan.outputDir).c_str());
		ImGui::PopTextWrapPos();
	}

	void BuildPanel::DrawScriptDebuggerRow(app::LayerContext& context)
	{
		auto* actions = context.TryGet<EditorProjectActions>();
		if (actions == nullptr)
		{
			return;
		}

		const auto& visualStudios = actions->visualStudioInstallations;
		const auto selected = std::ranges::find(visualStudios, m_visualStudioInstall, &VisualStudioInstallation::installPath);
		if (selected == visualStudios.end() && !visualStudios.empty())
		{
			const auto compatible = std::ranges::find_if(visualStudios, [](const VisualStudioInstallation& installation) { return installation.supportsDotNet10 && installation.hasDebuggerAutomation; });
			m_visualStudioInstall = (compatible != visualStudios.end() ? compatible : visualStudios.begin())->installPath;
		}
		const auto current = std::ranges::find(visualStudios, m_visualStudioInstall, &VisualStudioInstallation::installPath);

		// Every entry in this list is a Visual Studio, so the words "Visual Studio" are the
		// least informative part of each name and the first to cost the edition and year their
		// room. The full name is still used in messages, where there is space for it.
		const auto shortName = [](const std::string& name) -> std::string
		{
			constexpr std::string_view kPrefix = "Visual Studio ";
			return name.starts_with(kPrefix) ? name.substr(kPrefix.size()) : name;
		};

		ImGui::SeparatorText("Scripting");
		iw::LabelColumn("C# Debugger");
		const char* debugLabel = ICON_FA_BUG " Debug C#";
		const float debugWidth = ImGui::CalcTextSize(debugLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ImGui::SetNextItemWidth(-(debugWidth + ImGui::GetStyle().ItemSpacing.x));
		const std::string preview = current != visualStudios.end() ? shortName(current->displayName) : std::string("No Visual Studio IDE found");
		if (ImGui::BeginCombo("##scriptDebugger", preview.c_str()))
		{
			for (const VisualStudioInstallation& installation: visualStudios)
			{
				const bool isSelected = installation.installPath == m_visualStudioInstall;
				const std::string label = shortName(installation.displayName) + (installation.supportsDotNet10 ? "" : " (.NET 10 unsupported)") + (installation.hasDebuggerAutomation ? "" : " (debugger automation unavailable)");
				if (ImGui::Selectable(label.c_str(), isSelected))
				{
					m_visualStudioInstall = installation.installPath;
				}
				if (isSelected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		// The install path used to be part of the name, which is what made it too long to
		// read. It still matters when two installs share a product name, so it lives here.
		if (current != visualStudios.end())
		{
			ImGui::SetItemTooltip("%s", current->installPath.string().c_str());
		}
		ImGui::SameLine();
		const bool canDebugScripts = actions->debugScripts && current != visualStudios.end() && current->supportsDotNet10 && current->hasDebuggerAutomation;
		ImGui::BeginDisabled(!canDebugScripts);
		if (chrome::PrimaryButton(debugLabel))
		{
			SetStatus(actions->debugScripts(m_visualStudioInstall));
		}
		ImGui::EndDisabled();
	}

	void BuildPanel::DrawActionRow(app::LayerContext& context, const app::EditorProjectContext& project)
	{
		auto* actions = context.TryGet<EditorProjectActions>();
		if (actions == nullptr)
		{
			return;
		}

		ImGui::SeparatorText("Build");
		const bool publishing = m_publishFuture.valid();
		ButtonRow row;

		const char* packLabel = ICON_FA_BOX_OPEN " Pack Project";
		row.Item(packLabel);
		ImGui::BeginDisabled(!actions->packProject || publishing);
		if (chrome::GhostButton(packLabel))
		{
			SetStatus(actions->packProject(project));
		}
		ImGui::EndDisabled();

		if (actions->rebuildEnginePak)
		{
			const char* label = ICON_FA_GEAR " Rebuild Engine Pak";
			row.Item(label);
			ImGui::BeginDisabled(publishing);
			if (chrome::GhostButton(label))
			{
				SetStatus(actions->rebuildEnginePak());
			}
			ImGui::EndDisabled();
		}

		if (actions->recompileShaders)
		{
			const char* label = ICON_FA_BOLT " Recompile Shaders";
			row.Item(label);
			ImGui::BeginDisabled(publishing);
			if (chrome::GhostButton(label))
			{
				SetStatus(actions->recompileShaders());
			}
			ImGui::EndDisabled();
		}

		const char* publishLabel = ICON_FA_ROCKET " Publish";
		row.Item(publishLabel);
		ImGui::BeginDisabled(!actions->publishProject || publishing);
		if (chrome::PrimaryButton(publishLabel))
		{
			m_status = {};
			m_publishTask = std::make_shared<PublishTask>();
			const auto publishAction = actions->publishProject;
			const app::EditorProjectContext projectCopy = project;
			const std::shared_ptr<PublishTask> task = m_publishTask;
			m_publishFuture = std::async(std::launch::async,
			        [publishAction, projectCopy, task]()
			        {
				        return publishAction(projectCopy,
				                [task](const float completion, const std::string_view stage)
				                {
					                task->completion.store(completion, std::memory_order_release);
					                const std::scoped_lock lock(task->mutex);
					                task->stage = std::string(stage);
				                });
			        });
		}
		ImGui::EndDisabled();
	}

	void BuildPanel::DrawPublishProgress()
	{
		if (m_publishTask == nullptr)
		{
			return;
		}
		std::string stage;
		{
			const std::scoped_lock lock(m_publishTask->mutex);
			stage = m_publishTask->stage;
		}
		ImGui::Dummy(ImVec2(0.0f, 4.0f));
		ImGui::TextDisabled("%s", stage.empty() ? "Publishing..." : stage.c_str());
		ImGui::ProgressBar(m_publishTask->completion.load(std::memory_order_acquire), ImVec2(-FLT_MIN, 0.0f));
	}

	void BuildPanel::PollPublish(app::LayerContext& context)
	{
		if (!m_publishFuture.valid())
		{
			return;
		}
		// The publish itself runs on a worker, but this poll is what notices it finished and
		// what keeps the progress readable. An idle editor would check once every idle
		// interval and show a frozen panel until then.
		if (auto* engine = context.TryGet<AetherCore>())
		{
			engine->RequestActivity();
		}
		if (m_publishFuture.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
		{
			return;
		}
		EditorProjectActionResult result;
		try
		{
			result = m_publishFuture.get();
		}
		catch (const std::exception& ex)
		{
			result = {.succeeded = false, .message = std::string("Publishing failed: ") + ex.what()};
		}
		m_publishTask.reset();
		SetStatus(result);
		m_lastPublishSucceeded = result.succeeded;
		if (result.succeeded)
		{
			m_lastPublishPath = result.outputPath;
		}
	}

	void BuildPanel::DrawStatus()
	{
		if (!m_status.message.empty())
		{
			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(m_status.ok ? chrome::kSuccess : chrome::kError, "%s", m_status.message.c_str());
			if (!m_status.remediation.empty())
			{
				ImGui::TextDisabled("%s", m_status.remediation.c_str());
			}
			ImGui::PopTextWrapPos();
		}

		if (m_lastPublishSucceeded && !m_lastPublishPath.empty())
		{
			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			ImGui::BeginDisabled(!FolderExists(m_lastPublishPath));
			if (chrome::GhostButton(ICON_FA_FOLDER_OPEN " Open Build"))
			{
				OpenFolderInShell(m_lastPublishPath);
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			const std::string exeName = CurrentPublishEnvironment().runtimeExeName;
			std::error_code ec;
			const bool exeReady = std::filesystem::exists(m_lastPublishPath / exeName, ec);
			ImGui::BeginDisabled(!exeReady);
			if (chrome::GhostButton(ICON_FA_PLAY " Run Build", ImVec2(0.0f, 0.0f), chrome::kAccentHi))
			{
				LaunchGameBuild(m_lastPublishPath, exeName);
			}
			ImGui::EndDisabled();
			ImGui::SetItemTooltip("Launch the published game (working dir = build folder)");

			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("%s", DisplayPath(m_lastPublishPath).c_str());
			ImGui::PopTextWrapPos();
		}
	}

	void BuildPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Build", VisiblePtr());

		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			chrome::PanelHeader("BUILD");
			ImGui::TextDisabled("No project is open.");
			ImGui::End();
			return;
		}

		chrome::PanelHeader("BUILD", project->name.c_str());
		DrawConfigBanner(*project);
		DrawScriptDebuggerRow(context);
		DrawActionRow(context, *project);
		PollPublish(context);
		DrawPublishProgress();
		DrawStatus();

		ImGui::End();
	}
} // namespace aether::editor
