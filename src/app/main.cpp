#include <exception>
#include <memory>

#include "AetherExceptions.hpp"
#include "Application.hpp"
#include "CrashHandler.hpp"
#include "layers/DebugLayer.hpp"
#include "layers/SandboxLayer.hpp"
#include "Logger.hpp"

namespace
{
	// This is just a simple RAII guard to ensure that the logger and crashhandler
	// are properly shutdown when the application exits, even if an exception is
	// thrown.
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

int main()
{
	RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		aether::Logger::SetMinimumLevel(aether::LogLevel::Verbose);

		aether::app::Application application;
		application.PushLayer(std::make_unique<aether::app::SandboxLayer>());
		application.PushLayer(std::make_unique<aether::app::DebugLayer>());
		// Add more layers here as needed, like a editor layer or some kind of
		// background layer even
		return application.Run();
	}
	catch (const std::exception& exception)
	{
		const auto* engineError = dynamic_cast<const aether::EngineError*>(&exception);
		const aether::LogCategory category = engineError != nullptr ? engineError->Category() : aether::LogCategory::Std;
		if (category == aether::LogCategory::Vulkan)
		{
			aether::CrashHandler::ReportGraphicsFault("UnhandledVulkanException", exception.what());
		}
		ERROR(category, "Unhandled exception: {}", exception.what());
		return -1;
	}
	catch (...)
	{
		ERROR(aether::LogCategory::Unknown, "Unknown non-standard exception.");
		return -1;
	}
}
