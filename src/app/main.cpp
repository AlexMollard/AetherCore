#include <exception>
#include <memory>

#include "utils/AetherExceptions.hpp"
#include "Application.hpp"
#include "platform/CrashHandler.hpp"
#include "layers/DebugLayer.hpp"
// #include "layers/FishingLayer.hpp"
#include "layers/LoadingLayer.hpp"
#include "layers/ScriptedSceneLayer.hpp"
// #include "layers/SandboxLayer.hpp"
// #include "layers/VoxelWorldLayer.hpp"
// #include "layers/PhysicsLayer.hpp"
// #include "layers/UiSandboxLayer.hpp"
// #include "layers/InventoryLayer.hpp"
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

		aether::app::scripting::ScriptingSubsystem scriptingSubsystem;

		aether::app::SystemFactory systemFactory;

		aether::app::Application application;
		application.GetEngine().GetServiceContainer().Register<aether::app::scripting::ScriptingSubsystem>(scriptingSubsystem);

		// application.PushLayer(std::make_unique<aether::app::FishingLayer>());
		application.PushLayer(std::make_unique<aether::app::LoadingLayer>());
		application.PushLayer(std::make_unique<aether::app::ScriptedSceneLayer>("game.das", std::move(systemFactory)));
		// application.PushLayer(std::make_unique<aether::app::VoxelWorldLayer>());
		// application.PushLayer(std::make_unique<aether::app::PhysicsLayer>());
		// application.PushLayer(std::make_unique<aether::app::UiSandboxLayer>());
		// application.PushLayer(std::make_unique<aether::app::InventoryLayer>());
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
		AE_ERROR(category, "Unhandled exception: {}", exception.what());
		return -1;
	}
	catch (...)
	{
		AE_ERROR(aether::LogCategory::Unknown, "Unknown non-standard exception.");
		return -1;
	}
}
