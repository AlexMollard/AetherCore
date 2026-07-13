#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include "ProjectCli.hpp"
#include "utils/AetherExceptions.hpp"
#include "Application.hpp"
#include "platform/CrashHandler.hpp"
#include "platform/Window.hpp"
#include "layers/ScriptedSceneLayer.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/Logger.hpp"

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
	};
} // namespace

int main(int argc, char** argv)
{
	// --project <path> (passed by the Launcher when it spawns the editor, or by Visual
	// Studio for direct F5 debugging) boots straight into that project. Surface it as
	// AETHER_PROJECT_DIR - the shared env var the editor boot and the FileSystem project
	// mount already read - so nothing has to thread through the engine config.
	const std::string project = aether::app::ParseProjectArg(argc, argv);
	if (!project.empty())
	{
#ifdef _WIN32
		_putenv_s("AETHER_PROJECT_DIR", project.c_str());
#else
		setenv("AETHER_PROJECT_DIR", project.c_str(), 1);
#endif
	}
#if defined(AETHERCORE_EDITOR_APP) && !defined(AETHERCORE_LAUNCHER)
	else
	{
		// The editor is always project-scoped: launch it from the Launcher, or pass
		// --project directly (Visual Studio F5 does). Refuse a launcher-less editor.
		std::fprintf(stderr, "Editor requires --project <path>. Launch it from the Launcher.\n");
		return 2;
	}
#endif

	// Before any windowing/GLFW init, so a high-DPI monitor isn't virtualized down
	// (a 1440p @ 133% display would otherwise render + present at 1080p and upscale).
	aether::Window::EnableHighDpiAwareness();

	RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		aether::Logger::SetMinimumLevel(aether::LogLevel::Info);

		// NVIDIA Aftermath (dev-only GPU crash diagnostics) is only ever turned
		// on for an editor build - see the vendor + editor gate in
		// VulkanContext.cpp. GameRuntime leaves this false, so a shipped game
		// never enables it (and never needs GFSDK_Aftermath_Lib.x64.dll).
		aether::AetherCore::Config engineConfig{};
#ifdef AETHERCORE_EDITOR_APP
		engineConfig.enableGpuDiagnostics = true;
#endif
		aether::app::Application application(engineConfig);

		// Services
		// C# scripting: boots CoreCLR and loads the game-scripts assembly.
		// Disabled-safe - a missing runtime just means no entity-script behavior.
		aether::app::scripting::CSharpScriptingSubsystem csharpScripting;
		application.AddService(csharpScripting);

		// Layers
#ifdef AETHERCORE_EDITOR_APP
		application.PushLayer<aether::app::DebugLayer>();
		// Editor control endpoint (dormant unless AETHER_CONTROL_PORT is set); lets
		// the AetherCore MCP / aether-ctl drive the live editor.
		application.PushLayer<aether::app::editor::ControlServerLayer>();
#endif
		// World content comes from the startup scene file (engine.toml
		// app.startupScene); behavior comes from entity scripts (ScriptComponent).
		application.PushLayer<aether::app::ScriptedSceneLayer>();

		return application.Run();
	}
	catch (const std::exception& exception)
	{
		const auto engineError = dynamic_cast<const aether::EngineError*>(&exception);
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
