#include <exception>
#include <memory>

#include "Application.hpp"
#include "Logger.hpp"
#include "MeowExceptions.hpp"
#include "layers/SandboxLayer.hpp"

namespace
{
	// This is just a simple RAII guard to ensure that the logger is properly shutdown when the application exits, even if an exception is thrown.
	class LoggerShutdownGuard
	{
	public:
		LoggerShutdownGuard()
		{
			meow::Logger::Initialize();
		}

		~LoggerShutdownGuard()
		{
			meow::Logger::Shutdown();
		}
	};
}

int main()
{
	LoggerShutdownGuard loggerShutdownGuard;

	try
	{
		meow::Logger::SetMinimumLevel(meow::LogLevel::Verbose);

		meow::app::Application application;
		application.PushLayer(std::make_unique<meow::app::SandboxLayer>());
		// Add more layers here as needed, like a editor layer or some kind of background layer even
		return application.Run();
	}
	catch (const std::exception& exception)
	{
		const auto* engineError = dynamic_cast<const meow::EngineError*>(&exception);
		const meow::LogCategory category = engineError != nullptr ? engineError->Category() : meow::LogCategory::Std;
		ERROR(category, "Unhandled exception: {}", exception.what());
		return -1;
	}
	catch (...)
	{
		ERROR(meow::LogCategory::Unknown, "Unknown non-standard exception.");
		return -1;
	}
}