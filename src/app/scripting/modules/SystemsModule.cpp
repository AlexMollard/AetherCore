#include "scripting/DasModuleBase.hpp"

#include "daScript/daScript.h"

#include "scripting/SystemFactory.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"

namespace
{
	using namespace aether::app::scripting;

	// register_system("SystemName")
	// Instantiates a C++ System via the SystemFactory and registers it with the
	// World for per-frame updates.
	void das_register_system(const char* name)
	{
		auto& ctx = ActiveContext();
		if (!ctx.systemFactory)
		{
			AE_WARN(aether::LogCategory::App, "register_system: no SystemFactory available");
			return;
		}
		auto system = ctx.systemFactory->Create(name);
		if (!system)
		{
			return;
		}
		ctx.registeredSystems.emplace_back(name);
		ctx.world->RegisterSystem(std::move(system));
	}

} // namespace

namespace aether::app::scripting
{
	struct SystemsModule : DasModuleBase
	{
		SystemsModule()
		      : DasModuleBase("ecs_systems")
		{
			das::ModuleLibrary lib(this);

			Bind<das_register_system>(lib, "register_system", SE::modifyExternal);

			verifyAotReady();
		}
	};
} // namespace aether::app::scripting

AETHER_DAS_MODULE(SystemsModule, aether::app::scripting)
