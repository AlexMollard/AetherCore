#include "scripting/interop/InteropCommon.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "net/CSharpRpcBridge.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetworkContext.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting::interop;

// Net.CallServer's native half. On a client the call is encoded and sent to the
// host, which invokes it there (NetworkReceiveSystem's Rpc case). Anywhere else -
// on the host, or in an unnetworked game - the host IS this process, so the same
// call runs locally.
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

	auto* network = ctx.services->TryGet<aether::net::NetworkContext>();
	const bool remote = network != nullptr && network->IsClient();
	// A client can only ask about an entity the host also knows; an unbound one has
	// no name on the wire, so the call would arrive addressed to nothing.
	const std::uint32_t netId = remote ? network->Session().NetIdFor(entity) : 0;

	// First script attached to the entity whose [NetRpc] table names this method
	// wins - mirrors NetScriptFields' "first entry wins" simplification for a
	// duplicate script type, and covers the overwhelmingly common one-script-per-
	// entity case exactly.
	for (std::size_t i = 0; i < scripts->scripts.size(); ++i)
	{
		const std::string& typeName = scripts->scripts[i].path;
		const int methodIndex = bridge.FindMethodIndex(typeName, methodName);
		if (methodIndex < 0)
		{
			continue;
		}

		if (remote && netId != 0)
		{
			// The method index is resolved from THIS peer's assembly; the host resolves
			// the script by type hash and bounds-checks the index against its own table,
			// so an assembly mismatch drops the call rather than invoking the wrong one.
			const std::vector<std::byte> packet = aether::net::EncodeRpc(netId,
			        aether::net::ScriptTypeHash(typeName), static_cast<std::uint16_t>(methodIndex), args);
			network->Transport().Send(aether::net::kInvalidConnection, aether::net::kChannelReliable, true, packet);
			return 1;
		}

		bridge.Invoke(entity, static_cast<std::uint32_t>(i), static_cast<std::uint16_t>(methodIndex), args);
		return 1;
	}
	return 0; // no script on this entity declares that RPC
}
