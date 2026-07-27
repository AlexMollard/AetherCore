#include "scripting/interop/InteropCommon.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/CSharpRpcBridge.hpp"
#include "net/NetInput.hpp"
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

// Net.SendInput's native half - the client->host input path. Structurally the twin
// of aether_net_call_rpc above and deliberately so: it resolves the same
// [NetRpc(Server)] declaration, on the same script, through the same cached bridge,
// and dispatches through the same Invoke. Only DELIVERY differs, and every one of
// those differences is here rather than in the RPC path:
//
//   - The handler must declare NetRpcTarget::Server. Input is client-to-host by
//     definition, so a method declaring Client or Multicast is refused rather than
//     re-routed - the same rule, and the same shared predicate, Net.CallServer uses.
//   - The local invoke happens on EVERY allowed route, including a client's. That is
//     client-side prediction: the owner acts on its own input immediately and does
//     not wait for the round trip. RouteInput, not this function, decides it.
//   - The send is PACED and DEDUPED (InputSendPacer), so an unchanged payload at 300
//     fps is not 300 packets, while a payload that changed - the single frame a jump
//     was pressed - always goes out at once.
//   - It goes UNRELIABLE, on kChannelInput. Reliable would be wrong twice over: a
//     retransmitted input is stale by the time it lands, and one lost packet would
//     head-of-line-block every later input behind it. The channel is separate from
//     kChannelSnapshot because ENet sequences unreliable delivery per channel, so
//     two independent streams sharing one would each discard the other's packets as
//     out of order. Sequence numbers ride along in the message and the host applies
//     only strictly-newer ones, so a reordered arrival is dropped rather than
//     applied - the failure mode ENET_PACKET_FLAG_UNSEQUENCED once caused here.
AE_SCRIPT_API std::int32_t aether_net_send_input(std::uint32_t entityId, const char* methodNameUtf8,
        const std::uint8_t* payloadBlob, std::int32_t payloadLen)
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
	const std::size_t payloadCount = (payloadLen > 0 && payloadBlob != nullptr) ? static_cast<std::size_t>(payloadLen)
	                                                                            : 0;
	const std::span<const std::byte> payload(reinterpret_cast<const std::byte*>(payloadBlob), payloadCount);

	auto* network = ctx.services->TryGet<aether::net::NetworkContext>();
	std::optional<aether::net::CSharpRpcBridge> ownedBridge;
	const aether::net::RpcBridge* bridgePtr = network != nullptr ? network->Rpcs() : nullptr;
	if (bridgePtr == nullptr)
	{
		bridgePtr = &ownedBridge.emplace(*scripting, *instances, *ctx.services);
	}
	const aether::net::RpcBridge& bridge = *bridgePtr;

	static const aether::net::NetSession kOfflineSession;
	const aether::net::NetSession& session = network != nullptr ? network->Session() : kOfflineSession;

	for (std::size_t i = 0; i < scripts->scripts.size(); ++i)
	{
		const std::string& typeName = scripts->scripts[i].path;
		const aether::net::RpcMethod method = bridge.FindMethod(typeName, methodName);
		if (!method.Found())
		{
			continue;
		}
		if (aether::net::RpcTargetMismatch(static_cast<std::int32_t>(aether::net::NetRpcTarget::Server),
		            method.target))
		{
			return 0; // the handler declares a host-to-client direction; input has none
		}

		const aether::net::InputRoute route = aether::net::RouteInput(world, session, entity);
		if (!route.allowed)
		{
			return 0;
		}

		if (route.send && network != nullptr)
		{
			const std::uint32_t netId = session.NetIdFor(entity);
			const aether::net::InputSend paced = network->InputPacer().Prepare(netId, payload, network->Now(),
			        network->InputSendRateHz());
			if (paced.send)
			{
				const std::vector<std::byte> packet = aether::net::EncodeInput(netId,
				        aether::net::ScriptTypeHash(typeName), static_cast<std::uint16_t>(method.index),
				        paced.sequence, payload);
				// kInvalidConnection is how the transport spells "the host".
				network->Transport().Send(aether::net::kInvalidConnection, aether::net::kChannelInput,
				        /*reliable=*/false, packet);
			}
		}

		if (route.invokeLocally)
		{
			bridge.Invoke(entity, static_cast<std::uint32_t>(i), static_cast<std::uint16_t>(method.index), payload);
		}
		return 1;
	}
	return 0; // no script on this entity declares that handler
}
