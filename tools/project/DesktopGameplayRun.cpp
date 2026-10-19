#include "DesktopGameplayRun.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#	define WIN32_LEAN_AND_MEAN
#	define NOMINMAX
#	include <windows.h>
#	include <psapi.h>
#endif

#include "Application.hpp"
#include "ProjectScripting.hpp"
#include "audio/AudioSubsystem.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "layers/ScriptedSceneLayer.hpp"
#include "platform/Input.hpp"
#include "platform/Window.hpp"
#include "rendering/ScreenshotService.hpp"
#include "project/ProjectCommon.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/RustScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::project_tool
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Json = nlohmann::json;
		using Status = aether::scripting::AgentJobStatus;
		using namespace app;
		using namespace app::scripting;

		void Require(bool condition, const std::string& message)
		{
			if (!condition)
			{
				throw std::runtime_error(message);
			}
		}

		void Environment(const char* name, const char* value)
		{
#ifdef _WIN32
			_putenv_s(name, value);
#else
			setenv(name, value, 1);
#endif
		}

		Json Samples(std::vector<double> values)
		{
			if (values.empty())
			{
				return nullptr;
			}
			std::sort(values.begin(), values.end());
			return {{"count", values.size()}, {"medianMs", values[values.size() / 2]}, {"p95Ms", values[std::min(values.size() - 1, values.size() * 95 / 100)]}};
		}

		Json Memory()
		{
#ifdef _WIN32
			PROCESS_MEMORY_COUNTERS_EX counters{};
			if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
			{
				return {{"residentBytes", counters.WorkingSetSize}, {"peakResidentBytes", counters.PeakWorkingSetSize}, {"privateBytes", counters.PrivateUsage}};
			}
#else
			std::ifstream status("/proc/self/status");
			std::string line;
			Json result = Json::object();
			while (std::getline(status, line))
			{
				if (line.starts_with("VmRSS:"))
				{
					result["residentBytes"] = std::stoull(line.substr(6)) * 1024;
				}
				if (line.starts_with("VmHWM:"))
				{
					result["peakResidentBytes"] = std::stoull(line.substr(6)) * 1024;
				}
			}
			if (!result.empty())
			{
				return result;
			}
#endif
			return nullptr;
		}

		class FixedStepApplication final : public Application
		{
		public:
			FixedStepApplication(const AetherCore::Config& config, float step)
			      : Application(config), m_step(step)
			{
			}

			void OnUpdate(double, std::uint64_t frame) override
			{
				const auto started = Clock::now();
				Application::OnUpdate(m_step, frame);
				updateTimes.push_back(std::chrono::duration<double, std::milli>(Clock::now() - started).count());
			}

			std::vector<double> updateTimes;

		private:
			float m_step;
		};

		class DesktopDriver final : public AppLayer
		{
		public:
			DesktopDriver(const GameplayRunRequest& request, Clock::time_point started, std::vector<std::string>& errors)
			      : m_request(request), m_started(started), m_errors(errors)
			{
			}

			void OnAttach(LayerContext& context) override
			{
				context.Get<SettingsService>().Values().app.startupScene = m_request.scene;
				context.Get<PlayState>().SetMode(PlayState::Mode::Playing);
				context.Get<GameplayHost>().SetErrorHandler([this](const std::string& message) { m_errors.push_back(message); });
			}

			void OnBeforeInput(LayerContext& context) override
			{
				context.Get<SceneContext>().allowSaveReads = m_request.launchConfig.value("readSaves", false);
				for (const auto& event: m_request.launchConfig.value("input", Json::array()))
				{
					if (event["tick"].get<std::uint32_t>() == m_tick)
					{
						context.Get<Input>().SetSyntheticKey(Input::KeyCodeFromName(event["key"].get<std::string>()), event["down"].get<bool>(), false);
					}
				}
			}

			void OnUpdate(LayerContext& context) override
			{
				const auto now = Clock::now();
				if (m_tick == 0)
				{
					startupMs = std::chrono::duration<double, std::milli>(now - m_started).count();
				}
				else
				{
					frameTimes.push_back(std::chrono::duration<double, std::milli>(now - m_previous).count());
				}
				m_previous = now;
				Require(context.Get<PlayState>().IsPlaying(), "Desktop test left Playing unexpectedly");
				Require(m_errors.empty(), m_errors.empty() ? "" : m_errors.front());
				if (auto* managed = context.TryGet<CSharpScriptingSubsystem>())
				{
					Require(managed->ScriptErrors().empty(), managed->ScriptErrors().empty() ? "" : managed->ScriptErrors().front());
				}
				if (auto* rust = context.TryGet<RustScriptingSubsystem>())
				{
					for (const auto& diagnostic: rust->Host().Diagnostics())
					{
						Require(diagnostic.severity != aether::scripting::AgentDiagnosticSeverity::Error, diagnostic.message);
					}
				}
				if (m_tick == 0)
				{
					const auto epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
					const Json ready = {{"schemaVersion", 1}, {"firstTickUnixNs", epoch}};
					Require(io::file_util::WriteText(m_request.output.parent_path() / "ready.json", ready.dump()).has_value(), "Cannot write first-tick readiness receipt");
				}
				++m_tick;
				if (m_tick == 10 && m_request.ticks >= 20)
				{
					m_capture = context.Get<ScreenshotService>().Request(io::PlatformPaths::PathToUtf8(m_request.output.parent_path() / "reference.png"));
				}
				const auto audioStats = context.Get<audio::AudioSubsystem>().GetStats();
				const Json observations = Json::array({{{"entity", "$runtime"}, {"type", "$engine"}, {"properties", {{"scene", context.Get<SceneSubsystem>().GetCurrentScene()}, {"audioStarted", audioStats.startedTotal}}}}});
				for (const auto& checkpoint: m_request.launchConfig.value("checkpoints", Json::array()))
				{
					if (checkpoint["tick"].get<std::uint32_t>() != m_tick)
					{
						continue;
					}
					auto capture = m_request;
					capture.ticks = m_tick;
					capture.output = m_request.output.parent_path() / ("checkpoint-" + std::to_string(m_tick) + ".json");
					capture.launchConfig["assertions"] = checkpoint["assertions"];
					WriteGameplayState(capture, context.Get<World>(), context.Get<GameplayHost>(), context.Get<ScriptComponentSystem>(), context.Get<SceneContext>().allowSaveReads, observations);
				}
				if (m_tick == m_request.ticks)
				{
					WriteGameplayState(m_request, context.Get<World>(), context.Get<GameplayHost>(), context.Get<ScriptComponentSystem>(), context.Get<SceneContext>().allowSaveReads, observations);
					const auto text = io::file_util::ReadText(m_request.output);
					Require(text.has_value(), "Cannot read desktop state artifact");
					auto state = Json::parse(*text);
					state["profile"] = "native-desktop-2d-v1";
					state["scene"] = context.Get<SceneSubsystem>().GetCurrentScene();
					const auto audio = context.Get<audio::AudioSubsystem>().GetStats();
					state["audio"] = {{"startedVoices", audio.startedTotal}, {"activeVoices", audio.activeVoices}};
					state["memory"] = Memory();
					Require(io::file_util::WriteText(m_request.output, state.dump(2)).has_value(), "Cannot write desktop telemetry");
					if (m_capture.valid())
					{
						Require(m_capture.wait_for(std::chrono::seconds(0)) == std::future_status::ready && !m_capture.get().empty(), "Desktop render capture did not complete");
					}
					completed = true;
					context.Get<Window>().RequestClose();
				}
			}

			bool completed = false;
			double startupMs = 0;
			std::vector<double> frameTimes;

		private:
			const GameplayRunRequest& m_request;
			Clock::time_point m_started;
			Clock::time_point m_previous;
			std::vector<std::string>& m_errors;
			std::uint32_t m_tick = 0;
			std::future<std::string> m_capture;
		};
	} // namespace

	aether::scripting::AgentReport RunDesktopGameplay(const app::scripting::GameplayRunRequest& request)
	{
		const auto started = Clock::now();
		aether::scripting::AgentReport report{.jobId = "desktop-run", .status = Status::Failed, .phase = "run", .target = "desktop", .profile = "native-desktop-2d-v1"};
		try
		{
			ValidateGameplayRunConfig(request);
			Require(request.launchConfig.value("profile", "") == "native-desktop-2d-v1", "Desktop runner requires native-desktop-2d-v1");
			Require(scene::IsValidAssetName(request.scene) && !request.output.empty(), "A named scene and state output are required");
			const auto descriptor = project::ReadProjectDescriptor(request.project);
			Require(descriptor.has_value(), descriptor ? "" : descriptor.error().message);
			report.language = std::string(GameplayLanguageName(descriptor->language));
			Environment("AETHER_AGENT_SESSION", "1");
			Environment("AETHER_AGENT_BACKGROUND", "1");
			Environment("AETHER_AGENT_NAME", "Gameplay comparison");
			Environment("AETHER_CONTROL_PORT", "off");
			ProjectScriptingSettings selected{descriptor->language};
			std::vector<std::string> errors;
			std::unique_ptr<CSharpScriptingSubsystem> managed;
			std::unique_ptr<RustScriptingSubsystem> rust;
			AetherCore::Config config{};
			config.startWindowHidden = true;
			FixedStepApplication application(config, request.step);
			application.SetSettingsPersistenceEnabled(false);
			application.AddService(selected);
			if (selected.language == GameplayLanguage::Rust)
			{
				rust = std::make_unique<RustScriptingSubsystem>(request.project,
				        [&](const RustBuildRequest&)
				        {
					        RustBuildResult result;
					        result.module = request.module;
					        result.report.status = Status::Passed;
					        return result;
				        });
				application.AddService(*rust);
				application.AddService(rust->Gameplay());
			}
			else
			{
				Require(std::filesystem::is_regular_file(request.managedDir / "AetherGame.dll"), "Isolated managed assembly missing");
				managed = std::make_unique<CSharpScriptingSubsystem>(request.managedDir);
				Require(managed->IsAvailable(), "Managed host unavailable");
				application.AddService(*managed);
				application.AddService(managed->Gameplay());
			}
			auto& driver = application.PushLayer<DesktopDriver>(request, started, errors);
			application.PushLayer<ScriptedSceneLayer>();
			const int exitCode = application.Run();
			Require(exitCode == 0 && driver.completed, "Desktop gameplay closed before completing the bounded run");
			// Detach while the scene and all native services still exist, and observe
			// its errors before success. Application teardown then has no live scripts.
			application.Services().Get<ScriptComponentSystem>().Invalidate(application.Services().Get<World>());
			if (rust)
			{
				rust->Unload();
			}
			Require(errors.empty(), errors.empty() ? "" : errors.front());
			if (managed)
			{
				Require(managed->ScriptErrors().empty(), managed->ScriptErrors().empty() ? "" : managed->ScriptErrors().front());
			}
			if (rust)
			{
				for (const auto& diagnostic: rust->Host().Diagnostics())
				{
					Require(diagnostic.severity != aether::scripting::AgentDiagnosticSeverity::Error, diagnostic.message);
				}
			}
			const auto text = io::file_util::ReadText(request.output);
			Require(text.has_value(), "Desktop state artifact missing");
			auto state = Json::parse(*text);
			state["timing"] = {{"startupToFirstTickMs", driver.startupMs}, {"cpuUpdate", Samples(application.updateTimes)}, {"frameInterval", Samples(driver.frameTimes)}, {"gpuTimingMeasured", false}};
			Require(io::file_util::WriteText(request.output, state.dump(2)).has_value(), "Cannot write desktop timing");
			report.status = Status::Passed;
			report.requiredChecks = {{"desktop-runtime", Status::Passed}, {"fixed-gameplay-ticks", Status::Passed}, {"state-assertions", Status::Passed}};
		}
		catch (const std::exception& error)
		{
			report.diagnostics.push_back({"DESKTOP_GAMEPLAY_RUN", aether::scripting::AgentDiagnosticSeverity::Error, error.what()});
		}
		if (std::filesystem::is_regular_file(request.output))
		{
			report.artifacts.push_back({io::PlatformPaths::PathToUtf8(request.output), std::filesystem::file_size(request.output)});
		}
		report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
		return report;
	}
} // namespace aether::project_tool
