#include <exception>
#include <memory>

#include "Application.hpp"
#include "CrashHandler.hpp"
#include "Logger.hpp"
#include "MeowExceptions.hpp"
#include "layers/SandboxLayer.hpp"
#include "layers/UILayer.hpp"

namespace
{
	// This is just a simple RAII guard to ensure that the logger and crashhandler are properly shutdown when the application exits, even if an exception is thrown.
	class RuntimeSystemsGuard
	{
	public:
		RuntimeSystemsGuard()
		{
			meow::Logger::Initialize();
			meow::CrashHandler::Install("MeowCore");
		}

		~RuntimeSystemsGuard()
		{
			meow::CrashHandler::Uninstall();
			meow::Logger::Shutdown();
		}
	};
}

int main()
{
	RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		meow::Logger::SetMinimumLevel(meow::LogLevel::Verbose);

		meow::app::Application application;
		application.PushLayer(std::make_unique<meow::app::SandboxLayer>());
		application.PushLayer(std::make_unique<meow::app::UILayer>());
		// Add more layers here as needed, like a editor layer or some kind of background layer even
		return application.Run();
	}
	catch (const std::exception& exception)
	{
		const auto* engineError = dynamic_cast<const meow::EngineError*>(&exception);
		const meow::LogCategory category = engineError != nullptr ? engineError->Category() : meow::LogCategory::Std;
		if (category == meow::LogCategory::Vulkan)
		{
			meow::CrashHandler::ReportGraphicsFault("UnhandledVulkanException", exception.what());
		}
		ERROR(category, "Unhandled exception: {}", exception.what());
		return -1;
	}
	catch (...)
	{
		ERROR(meow::LogCategory::Unknown, "Unknown non-standard exception.");
		return -1;
	}
}