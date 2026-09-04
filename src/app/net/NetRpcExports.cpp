#include "scripting/interop/InteropCommon.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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

// Net.Call's native half, and the framework's ONLY RPC send path. The method's
// [NetRpc] attribute states where it runs; this resolves that target, asks RouteRpc
// where the call therefore goes, and then does exactly what it was told - the policy
// (including "a client may not originate a multicast") lives in RouteRpc, which is
// pure and unit-tested, not here.
//
// `expectedTarget` is -1 for "whatever the method declares"; Net.CallServer passes a
// NetRpcTarget value the declaration must match, so the explicit spelling cannot
// silently dispatch a method that declares something else.
AE_SCRIPT_API std::int32_t aether_net_call_rpc(std::uint32_t entityId, const char* methodNameUtf8,
        const std::uint8_t* argBlob, std::int32_t argLen, std::int32_t expectedTarget)
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

	const std::string methodName = methodNameUtf8;
	const std::size_t argCount = (argLen > 0 && argBlob != nullptr) ? static_cast<std::size_t>(argLen) : 0;
	const std::span<const std::byte> args(reinterpret_cast<const std::byte*>(argBlob), argCount);

	auto* network = ctx.services->TryGet<aether::net::NetworkContext>();
	// Borrow the context's cached bridge - it holds the per-type [NetRpc] method
	// table a per-call temporary would rebuild. A single-player build registers no
	// NetworkContext at all, so fall back to a local one for the offline path.
	std::optional<aether::net::CSharpRpcBridge> ownedBridge;
	const aether::net::RpcBridge* bridgePtr = network != nullptr ? network->Rpcs() : nullptr;
	if (bridgePtr == nullptr)
	{
		bridgePtr = &ownedBridge.emplace(*scripting, *instances, *ctx.services);
	}
	const aether::net::RpcBridge& bridge = *bridgePtr;

	// No NetworkContext at all (a single-player build) is the offline case, which a
	// default-constructed session already describes: role Offline, so RouteRpc answers
	// "run it here" and never asks for a recipient.
	static const aether::net::NetSession kOfflineSession;
	const aether::net::NetSession& session = network != nullptr ? network->Session() : kOfflineSession;

	// First script attached to the entity whose [NetRpc] table names this method
	// wins - mirrors NetScriptFields' "first entry wins" simplification for a
	// duplicate script type, and covers the overwhelmingly common one-script-per-
	// entity case exactly.
	for (std::size_t i = 0; i < scripts->scripts.size(); ++i)
	{
		const std::string& typeName = scripts->scripts[i].path;
		const aether::net::RpcMethod method = bridge.FindMethod(typeName, methodName);
		if (!method.Found())
		{
			continue;
		}
		if (aether::net::RpcTargetMismatch(expectedTarget, method.target))
		{
			// Net.CallServer naming a method that declares Client or Multicast. The
			// rule lives in NetRpc.cpp so it is reachable from EngineTests - this TU is
			// CLR-linked and cannot be.
			return 0;
		}
		if (method.index > 0xFFFF)
		{
			// The wire field is u16; an index above it would wrap to a DIFFERENT
			// method, and the receiver's bounds-check cannot tell. Refuse the call.
			return 0;
		}

		const aether::net::RpcRoute route = aether::net::RouteRpc(world, session, method.target, entity);
		if (!route.allowed)
		{
			return 0;
		}

		if (!route.recipients.empty() && network != nullptr)
		{
			// The method index is resolved from THIS peer's assembly; the receiver
			// resolves the script by type hash and bounds-checks the index against its
			// own table, so an assembly mismatch drops the call rather than invoking the
			// wrong one. Encoded once and sent to each recipient - a multicast to eight
			// connections is one buffer, not eight.
			const std::vector<std::byte> packet = aether::net::EncodeRpc(session.NetIdFor(entity),
			        aether::net::ScriptTypeHash(typeName), static_cast<std::uint16_t>(method.index), method.target,
			        args);
			for (const aether::net::ConnectionId recipient: route.recipients)
			{
				network->Transport().Send(recipient, aether::net::kChannelReliable, true, packet);
			}
		}

		if (route.invokeLocally)
		{
			bridge.Invoke(entity, static_cast<std::uint32_t>(i), static_cast<std::uint16_t>(method.index), args);
		}
		return 1;
	}
	return 0; // no script on this entity declares that RPC
}
