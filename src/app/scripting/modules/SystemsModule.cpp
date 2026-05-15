#include "SystemsModule.hpp"
#include "DasHelpers.hpp"

#include "daScript/daScript.h"

#include "scripting/SystemFactory.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

namespace
{
	using namespace aether;
	using namespace aether::app::scripting;

	// register_system("SystemName") - instantiates a C++ System via the SystemFactory
	// and registers it with the World for per-frame updates.
	void das_register_system(const char* name)
	{
		auto& ctx = ActiveContext();
		if (!ctx.systemFactory)
		{
			WARN(LogCategory::App, "das register_system: no SystemFactory available");
			return;
		}
		auto system = ctx.systemFactory->Create(name);
		if (!system)
		{
			return; // SystemFactory already logged the warning
		}
		ctx.registeredSystems.emplace_back(name);
		ctx.world->RegisterSystem(std::move(system));
	}

} // namespace

namespace aether::app::scripting
{
	struct SystemsModule : das::Module
	{
		SystemsModule()
		      : das::Module("ecs_systems")
		{
			das::ModuleLibrary lib(this);

			addExtern<DAS_BIND_FUN(das_register_system)>(*this, lib, "register_system", das::SideEffects::modifyExternal, "das_register_system");

			verifyAotReady();
		}
	};

} // namespace aether::app::scripting

REGISTER_MODULE_IN_NAMESPACE(SystemsModule, aether::app::scripting);

void RegisterSystemsModule()
{
	NEED_MODULE(SystemsModule);
}
