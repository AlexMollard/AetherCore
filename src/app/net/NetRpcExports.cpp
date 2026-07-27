#include "scripting/interop/InteropCommon.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "net/CSharpRpcBridge.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting::interop;

// Net.CallServer's native half. There is no live NetSession/NetworkSubsystem wired
// into a running App yet - both are still standalone, testable pieces with no
// owning system (see net/NetSession.hpp, engine/net/NetworkSubsystem.hpp) - so a
// [NetRpc(Server)] call always runs locally today: correct when this process is
// already the host, or in an unnetworked game, and the seam a future NetSystem
// hooks to redirect a client's call over the wire instead.
AE_SCRIPT_API std::int32_t aether_net_call_server(std::uint32_t entityId, const char* methodNameUtf8,
        const std::uint8_t* argBlob, std::int32_t argLen)
{
	if (methodNameUtf8 == nullptr)
	{
		return 0;
	}

	auto& ctx = aether::app::scripting::ActiveContext();
	if (ctx.services == nullptr)
	{
		return 0;
	}
	auto* scripting = ctx.services->TryGet<aether::app::scripting::CSharpScriptingSubsystem>();
	auto* instances = ctx.services->TryGet<aether::app::ScriptComponentSystem>();
	if (scripting == nullptr || instances == nullptr)
	{
		return 0;
	}

	aether::World& world = ActiveWorld();
	const aether::Entity entity{entityId};
	const auto* scripts = world.TryGet<aether::ScriptComponent>(entity);
	if (scripts == nullptr)
	{
		return 0;
	}

	const aether::net::CSharpRpcBridge bridge{*scripting, *instances};
	const std::string methodName = methodNameUtf8;
	const std::size_t argCount = (argLen > 0 && argBlob != nullptr) ? static_cast<std::size_t>(argLen) : 0;
	const std::span<const std::byte> args(reinterpret_cast<const std::byte*>(argBlob), argCount);

	// First script attached to the entity whose [NetRpc] table names this method
	// wins - mirrors NetScriptFields' "first entry wins" simplification for a
	// duplicate script type, and covers the overwhelmingly common one-script-per-
	// entity case exactly.
	for (std::size_t i = 0; i < scripts->scripts.size(); ++i)
	{
		const int methodIndex = bridge.FindMethodIndex(scripts->scripts[i].path, methodName);
		if (methodIndex < 0)
		{
			continue;
		}
		bridge.Invoke(entity, static_cast<std::uint32_t>(i), static_cast<std::uint16_t>(methodIndex), args);
		return 1;
	}
	return 0; // no script on this entity declares that RPC
}
