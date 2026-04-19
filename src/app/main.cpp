#include <exception>
#include <memory>

#include "AetherExceptions.hpp"
#include "Application.hpp"
#include "CrashHandler.hpp"
#include "layers/DebugLayer.hpp"
// #include "layers/FishingLayer.hpp"
#include "layers/SandboxLayer.hpp"
// #include "layers/VoxelWorldLayer.hpp"
#include "Logger.hpp"

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

int main()
{
	RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		aether::Logger::SetMinimumLevel(aether::LogLevel::Verbose);

		aether::app::Application application;
		// application.PushLayer(std::make_unique<aether::app::FishingLayer>());
		application.PushLayer(std::make_unique<aether::app::SandboxLayer>());
		// application.PushLayer(std::make_unique<aether::app::VoxelWorldLayer>());
		application.PushLayer(std::make_unique<aether::app::DebugLayer>());
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
