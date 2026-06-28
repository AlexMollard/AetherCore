#include <exception>
#include <memory>

#include "utils/AetherExceptions.hpp"
#include "Application.hpp"
#include "platform/CrashHandler.hpp"
#include "layers/DebugLayer.hpp"
#include "layers/ScriptedSceneLayer.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "scripting/SystemFactory.hpp"
#include "utils/Logger.hpp"

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
		aether::Logger::SetMinimumLevel(aether::LogLevel::Info);

		aether::app::Application application;

		// Services
		aether::app::scripting::ScriptingSubsystem scriptingSubsystem;
		application.AddService(scriptingSubsystem);

		aether::app::SystemFactory systemFactory;
		application.AddService(systemFactory);

		// Layers
		application.PushLayer<aether::app::DebugLayer>();
		application.PushLayer<aether::app::ScriptedSceneLayer>("sandbox.das");

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
