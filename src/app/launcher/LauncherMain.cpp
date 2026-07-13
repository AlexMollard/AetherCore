#include <cstdlib>
#include <exception>

#include "Application.hpp"
#include "launcher/LauncherLayer.hpp"
#include "platform/CrashHandler.hpp"
#include "platform/Window.hpp"
#include "utils/AetherExceptions.hpp"
#include "utils/Logger.hpp"

namespace
{
	class RuntimeSystemsGuard
	{
	public:
		RuntimeSystemsGuard()
		{
			aether::Logger::Initialize();
			aether::CrashHandler::Install("AetherCoreLauncher");
		}

		~RuntimeSystemsGuard()
		{
			aether::CrashHandler::Uninstall();
			aether::Logger::Shutdown();
		}
	};
} // namespace

// Entry point for the AetherCore project Launcher - a Unity-Hub-style project picker.
// It is a UiShell app (window + Vulkan + Dear ImGui only; see RuntimeProfile.hpp),
// deliberately separate from the editor's main.cpp: no C# scripting host and no scene
// layers, just the LauncherLayer hub, which spawns a separate full-engine Editor
// process per project.
int main(int /*argc*/, char** /*argv*/)
{
	// Before any windowing/GLFW init so a high-DPI monitor isn't virtualized down -
	// mirrors the editor entry (see Window::EnableHighDpiAwareness).
	aether::Window::EnableHighDpiAwareness();

	RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		aether::Logger::SetMinimumLevel(aether::LogLevel::Info);

		aether::AetherCore::Config engineConfig{};
		// Dev-only GPU crash diagnostics, as for the editor (vendor/tooling gated in
		// VulkanContext.cpp; auto-disabled when the validation layer is active).
		engineConfig.enableGpuDiagnostics = true;
		// The whole point: bring up only window + device + ImGui overlay, no scene.
		engineConfig.profile = aether::RuntimeProfile::UiShell;

		aether::app::Application application(engineConfig);
		application.PushLayer<aether::app::LauncherLayer>();

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
