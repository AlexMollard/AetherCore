#include <exception>
#include <filesystem>
#include <memory>

#include "utils/AetherExceptions.hpp"
#include "Application.hpp"
#include "platform/CrashHandler.hpp"
#include "layers/DebugLayer.hpp"
#include "layers/ScriptedSceneLayer.hpp"
#include "scripting/DotNetHost.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "utils/Logger.hpp"

namespace
{
	// Locate the deployed managed assemblies (data/scripts/managed) relative to
	// the working directory, mirroring ScriptingSubsystem's script-path search.
	// TODO(phase1): fold this into CSharpScriptingSubsystem.
	std::filesystem::path ResolveManagedDir()
	{
		const auto cwd = std::filesystem::current_path();
		const std::filesystem::path candidates[] = {
			cwd / "data" / "scripts" / "managed",
			cwd / ".." / "data" / "scripts" / "managed",
			cwd / ".." / ".." / "data" / "scripts" / "managed",
		};
		for (const auto& dir: candidates)
		{
			if (std::filesystem::exists(dir / "AetherCore.Managed.dll"))
			{
				return dir;
			}
		}
		return candidates[0];
	}

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

		// Boot the .NET runtime for C# scripting. Disabled-safe: a missing runtime
		// logs a warning and leaves the host unavailable. TODO(phase1): own this
		// from CSharpScriptingSubsystem and register it as a service.
		aether::scripting::DotNetHost dotnetHost;
		dotnetHost.Initialize(ResolveManagedDir());

		// Layers
		application.PushLayer<aether::app::DebugLayer>();
		// Scene-only operation: world content comes from the startup scene
		// file (engine.toml app.startupScene), behavior from entity scripts
		// (ScriptComponent). Pass a .das path here to add a main script back.
		application.PushLayer<aether::app::ScriptedSceneLayer>("");

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
