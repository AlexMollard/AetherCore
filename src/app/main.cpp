#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <print>
#include <string>

#include "ProjectCli.hpp"
#include "RuntimeProjectSettings.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/AetherExceptions.hpp"
#include "Application.hpp"
#include "platform/CrashHandler.hpp"
#include "platform/Window.hpp"
#include "layers/ScriptedSceneLayer.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/RustScriptingSubsystem.hpp"
#include "utils/Logger.hpp"

#if defined(_WIN32)
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <Windows.h>
# include <shellapi.h>
# pragma comment(lib, "Shell32.lib")
#endif
#ifdef AETHERCORE_EDITOR_APP
#	include "editor/ControlServerLayer.hpp"
#	include "layers/DebugLayer.hpp"
#endif

namespace
{

	class RuntimeSystemsGuard
	{
	public:
		RuntimeSystemsGuard()
		{
			aether::Logger::Initialize();
			aether::CrashHandler::Install("AetherCore");
		}

		~RuntimeSystemsGuard()
		{
			aether::CrashHandler::Uninstall();
			aether::Logger::Shutdown();
		}

		RuntimeSystemsGuard(const RuntimeSystemsGuard&) = delete;
		RuntimeSystemsGuard& operator=(const RuntimeSystemsGuard&) = delete;
		RuntimeSystemsGuard(RuntimeSystemsGuard&&) = delete;
		RuntimeSystemsGuard& operator=(RuntimeSystemsGuard&&) = delete;
	};
} // namespace

int main(int argc, char** argv)
{
#if defined(AETHERCORE_EDITOR_APP) && defined(_WIN32)
	// MUST run before anything boots CoreCLR (the CSharpScriptingSubsystem service
	// below hosts the CLR): CoreCLR reads COMPlus_* configuration once, at CLR
	// startup, and CrashHandler::Install's unhandled-exception filter runs far too
	// late to matter for it - CoreCLR fail-fasts interop access violations through
	// its own vectored handler and calls TerminateProcess directly, so the filter
	// never sees them. That ordering IS the bug from commit f110d153: the Door
	// OnAttach segfault killed the process with zero minidump and zero handler log
	// (every dump in %LOCALAPPDATA%/AetherCore/crashes predated it), and the crash
	// was only diagnosed after enabling these same variables by hand. A silent
	// regression of this placement is indistinguishable from that blindness.
	//
	// Small dumps (type 1): native stacks + the exception context are the evidence
	// that matters; the full-heap variant captured ~190 MB for the crash above and
	// could not be opened by any tooling on the dev machine. Gated to editor builds
	// - interop crashes are a development-time phenomenon, and a published game
	// wants telemetry, not local dumps. An explicit COMPlus_DbgEnableMiniDump in the
	// environment is honoured, never stomped - that is the escape hatch (set =0 to
	// disable, or =2 for a full-heap dump when chasing heap state).
	if (std::getenv("COMPlus_DbgEnableMiniDump") == nullptr)
	{
		_putenv_s("COMPlus_DbgEnableMiniDump", "1");
		_putenv_s("COMPlus_DbgMiniDumpType", "1");
	}
#endif

#ifdef AETHERCORE_EDITOR_APP
	constexpr bool kRequireAgentControlPort = true;
#else
	constexpr bool kRequireAgentControlPort = false;
#endif
	auto agentLaunch = aether::app::ParseAgentLaunchOptions(argc, argv, kRequireAgentControlPort);
	if (!agentLaunch.error.empty())
	{
		std::println(stderr, "{}", agentLaunch.error);
		return 2;
	}
	if (agentLaunch.enabled)
	{
		// Establish the session before cached PlatformPaths queries, window creation,
		// audio initialization, autosave, or recovery can observe it.
#ifdef _WIN32
		// GLFW and the control protocol consume UTF-8. Windows' narrow argv uses
		// the active code page, so take the display name from the native command line.
		int wideCount = 0;
		wchar_t** wideArgs = CommandLineToArgvW(GetCommandLineW(), &wideCount);
		if (wideArgs == nullptr)
		{
			std::println(stderr, "Cannot read the agent name from the Windows command line.");
			return 2;
		}
		for (int i = 1; i + 1 < wideCount; ++i)
		{
			if (std::wstring_view(wideArgs[i]) == L"--agent-name")
			{
				const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArgs[i + 1], -1, nullptr, 0, nullptr, nullptr);
				if (size <= 1)
				{
					LocalFree(wideArgs);
					std::println(stderr, "Agent name must be valid Unicode and nonempty.");
					return 2;
				}
				agentLaunch.name.resize(static_cast<std::size_t>(size));
				WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wideArgs[i + 1], -1, agentLaunch.name.data(), size, nullptr, nullptr);
				agentLaunch.name.pop_back();
			}
		}
		LocalFree(wideArgs);
#endif
		const std::string controlPort = std::to_string(agentLaunch.controlPort);
#ifdef _WIN32
		_putenv_s("AETHER_AGENT_SESSION", "1");
		_putenv_s("AETHER_AGENT_BACKGROUND", agentLaunch.background ? "1" : "0");
		_putenv_s("AETHER_AGENT_NAME", agentLaunch.name.c_str());
		_putenv_s("AETHER_CONTROL_PORT", controlPort.c_str());
#else
		setenv("AETHER_AGENT_SESSION", "1", 1);
		setenv("AETHER_AGENT_BACKGROUND", agentLaunch.background ? "1" : "0", 1);
		setenv("AETHER_AGENT_NAME", agentLaunch.name.c_str(), 1);
		setenv("AETHER_CONTROL_PORT", controlPort.c_str(), 1);
#endif
	}
	// mount already read - so nothing has to thread through the engine config.
#ifdef _WIN32
	// Normal Launcher children need the same Unicode path fidelity as agent launches.
	int projectArgCount = 0;
	wchar_t** projectArgs = CommandLineToArgvW(GetCommandLineW(), &projectArgCount);
	if (projectArgs == nullptr)
	{
		std::println(stderr, "Cannot read the project path from the Windows command line.");
		return 2;
	}
	const std::wstring project = aether::app::ParseProjectArg(projectArgCount, projectArgs);
	LocalFree(projectArgs);
#else
	const std::string project = aether::app::ParseProjectArg(argc, argv);
#endif
	const std::string readyEvent = aether::app::ParseOptionArg(argc, argv, "--ready-event");
	// The Vulkan validation layer defaults to on for Debug and off for RelWithDebInfo.
	// --validation turns it on, --no-validation turns it off, and --no-validation wins if
	// both are passed - see ResolveValidationEnabled.
	const bool enableValidation = aether::app::ResolveValidationEnabled(argc, argv, AE_VALIDATION_DEFAULT_ON != 0);
	if (!project.empty())
	{
#ifdef _WIN32
		_wputenv_s(L"AETHER_PROJECT_DIR", project.c_str());
#else
		setenv("AETHER_PROJECT_DIR", project.c_str(), 1);
#endif
		aether::io::PlatformPaths::SetExplicitProjectDirectory(aether::io::PlatformPaths::ReadEnvironmentPath("AETHER_PROJECT_DIR"));
	}
	if (!readyEvent.empty())
	{
#ifdef _WIN32
		_putenv_s("AETHER_EDITOR_READY_EVENT", readyEvent.c_str());
#else
		setenv("AETHER_EDITOR_READY_EVENT", readyEvent.c_str(), 1);
#endif
	}
#ifdef AETHERCORE_EDITOR_APP
	if (project.empty())
	{
		// The editor is always project-scoped: launch it from the Launcher, or pass
		// --project directly (Visual Studio F5 does). Refuse a launcher-less editor.
		std::println(stderr, "Editor requires --project <path>. Launch it from the Launcher.");
		return 2;
	}
#endif

	// Before any windowing/GLFW init, so a high-DPI monitor isn't virtualized down
	// (a 1440p @ 133% display would otherwise render + present at 1080p and upscale).
	aether::Window::EnableHighDpiAwareness();

	const RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		aether::Logger::SetMinimumLevel(aether::LogLevel::Info);
		aether::app::RuntimeProjectSettingsInputs scriptingInputs;
		scriptingInputs.envProjectDir = aether::io::PlatformPaths::ReadEnvironmentPath("AETHER_PROJECT_DIR");
		scriptingInputs.exeDir = aether::io::PlatformPaths::GetExecutableDir();
#ifdef AETHER_DEFAULT_PROJECT_DIR
		scriptingInputs.compiledDefaultDir = AETHER_DEFAULT_PROJECT_DIR;
#endif
		auto selectedScripting = aether::app::ResolveRuntimeGameplaySettings(scriptingInputs);
		if (!selectedScripting)
		{
			AE_ERROR(aether::LogCategory::App, "Cannot select project gameplay language: {}", selectedScripting.error().message);
			return 2;
		}

		// NVIDIA Aftermath (dev-only GPU crash diagnostics) is only ever turned
		// on for an editor build - see the vendor + editor gate in
		// VulkanContext.cpp. GameRuntime leaves this false, so a shipped game
		// never enables it (and never needs GFSDK_Aftermath_Lib.x64.dll).
		aether::AetherCore::Config engineConfig{}; // NOLINT(misc-const-correctness): mutated only in the editor build below.
		engineConfig.enableValidation = enableValidation;
		// Started BY the launcher: stay unmapped until the project is loaded, then appear as
		// the launcher closes. Without this the editor window shows up empty the instant the
		// process starts and sits on top of the launcher - which is still there, because it
		// waits for the editor to report healthy - so opening a project flashes two windows.
		//
		// Only when the launcher started us. Run directly, a window that stays invisible for
		// several seconds reads as "it didn't launch", and there is nothing watching to say
		// otherwise.
		engineConfig.startWindowHidden = !readyEvent.empty() || agentLaunch.background;
		// "x,y" from the launcher: the point to centre on before the window is revealed.
		if (const std::string center = aether::app::ParseOptionArg(argc, argv, "--window-center"); !center.empty())
		{
			if (const std::size_t comma = center.find(','); comma != std::string::npos)
			{
				try
				{
					engineConfig.windowCenterX = std::stoi(center.substr(0, comma));
					engineConfig.windowCenterY = std::stoi(center.substr(comma + 1));
				}
				catch (const std::exception&)
				{
					// A malformed value is not worth refusing to start over; the window simply
					// lands wherever the OS puts it, which is the old behaviour.
				}
			}
		}
#ifdef AETHERCORE_EDITOR_APP
		engineConfig.enableGpuDiagnostics = true;
#endif
		// Registered services must remain alive through Application's layer teardown.
		std::unique_ptr<aether::app::scripting::CSharpScriptingSubsystem> csharpScripting;
		std::unique_ptr<aether::app::scripting::RustScriptingSubsystem> rustScripting;
		aether::app::Application application(engineConfig);

		// Services
		// C# scripting: boots CoreCLR and loads the game-scripts assembly.
		// Disabled-safe - a missing runtime just means no entity-script behavior.
		application.AddService(*selectedScripting);

		if (selectedScripting->language == aether::app::GameplayLanguage::CSharp)
		{
			csharpScripting = std::make_unique<aether::app::scripting::CSharpScriptingSubsystem>();
			application.AddService(*csharpScripting);
			application.AddService(csharpScripting->Gameplay());
		}
		else
		{
			auto sourceRoot = aether::io::PlatformPaths::IsIsolatedProduct()
			        ? aether::io::PlatformPaths::GetExplicitProjectDirectory() : scriptingInputs.envProjectDir;
			if (sourceRoot.empty() && !scriptingInputs.compiledDefaultDir.empty()
			        && aether::app::ResolveRuntimeProjectSettings(scriptingInputs).lexically_normal() == (scriptingInputs.compiledDefaultDir / "ProjectSettings.toml").lexically_normal())
			{
				sourceRoot = scriptingInputs.compiledDefaultDir;
			}
			rustScripting = std::make_unique<aether::app::scripting::RustScriptingSubsystem>(sourceRoot);
			application.AddService(*rustScripting);
			application.AddService(rustScripting->Gameplay());
		}

		// Layers
#ifdef AETHERCORE_EDITOR_APP
		application.PushLayer<aether::editor::DebugLayer>();
		// Editor control endpoint (dormant unless AETHER_CONTROL_PORT is set); lets
		// the AetherCore MCP / aether-ctl drive the live editor.
		application.PushLayer<aether::editor::ControlServerLayer>();
#endif
		// World content comes from the startup scene file (engine.toml
		// app.startupScene); behavior comes from entity scripts (ScriptComponent).
		application.PushLayer<aether::app::ScriptedSceneLayer>();

		return application.Run();
	}
	catch (const std::exception& exception)
	{
		const auto* const engineError = dynamic_cast<const aether::EngineError*>(&exception);
		const aether::LogCategory category = engineError != nullptr ? engineError->Category() : aether::LogCategory::Std;
		if (category == aether::LogCategory::Vulkan)
		{
			aether::CrashHandler::ReportGraphicsFault("UnhandledVulkanException", exception.what());
		}
		AE_ERROR(category, "Unhandled exception: {}", exception.what());
		return -1;
	}
	catch (...)
	{
		AE_ERROR(aether::LogCategory::Unknown, "Unknown non-standard exception.");
		return -1;
	}
}
