#include <exception>

#include "utils/AetherExceptions.hpp"
#include "Application.hpp"
#include "platform/CrashHandler.hpp"
#include "layers/ScriptedSceneLayer.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "utils/Logger.hpp"

#ifdef AETHERCORE_EDITOR_APP
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

int main()
{
	RuntimeSystemsGuard runtimeSystemsGuard;

	try
	{
		aether::Logger::SetMinimumLevel(aether::LogLevel::Info);

		aether::app::Application application;

		// Services
		// C# scripting: boots CoreCLR and loads the game-scripts assembly.
		// Disabled-safe - a missing runtime just means no entity-script behavior.
		aether::app::scripting::CSharpScriptingSubsystem csharpScripting;
		application.AddService(csharpScripting);

		// Layers
#ifdef AETHERCORE_EDITOR_APP
		application.PushLayer<aether::app::DebugLayer>();
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
