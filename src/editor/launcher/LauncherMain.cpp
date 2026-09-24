#include <cstdlib>
#include <exception>

#include "Application.hpp"
#include "debug/ProjectLauncherWindow.hpp"
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

		RuntimeSystemsGuard(const RuntimeSystemsGuard&) = delete;
		RuntimeSystemsGuard& operator=(const RuntimeSystemsGuard&) = delete;
		RuntimeSystemsGuard(RuntimeSystemsGuard&&) = delete;
		RuntimeSystemsGuard& operator=(RuntimeSystemsGuard&&) = delete;
	};
} // namespace

int main(int /*argc*/, char** /*argv*/)
{
	aether::Window::EnableHighDpiAwareness();

	const RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		aether::Logger::SetMinimumLevel(aether::LogLevel::Info);

		aether::AetherCore::Config engineConfig{};
		engineConfig.appName = "AetherCore Launcher";
		engineConfig.width = aether::app::kProjectLauncherDefaultWidth;
		engineConfig.height = aether::app::kProjectLauncherDefaultHeight;
		engineConfig.enableGpuDiagnostics = true;
		engineConfig.profile = aether::RuntimeProfile::UiShell;

		aether::app::Application application(engineConfig);
		application.PushLayer<aether::app::LauncherLayer>();

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
