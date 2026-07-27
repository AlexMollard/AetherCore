#include "net/NetRpc.hpp"

#include <algorithm>

#include "net/NetComponents.hpp"
#include "net/NetSession.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::net
{
	namespace
	{
		// The first script on `scripts` whose type name hashes to `typeHash`. Mirrors
		// NetScriptFields.cpp's FindScriptIndex: two scripts of the same type on one
		// entity are indistinguishable on the wire, so the first wins.
		std::optional<std::uint32_t> FindScriptIndex(const ScriptComponent& scripts, std::uint32_t typeHash)
		{
			for (std::size_t i = 0; i < scripts.scripts.size(); ++i)
			{
				if (ScriptTypeHash(scripts.scripts[i].path) == typeHash)
				{
					return static_cast<std::uint32_t>(i);
				}
			}
			return std::nullopt;
		}
	} // namespace

	std::vector<std::byte> EncodeRpc(std::uint32_t netId, std::uint32_t scriptTypeHash, std::uint16_t methodIndex,
	        NetRpcTarget target, std::span<const std::byte> args)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Rpc));
		w.U32(netId);
		w.U32(scriptTypeHash);
		w.U16(methodIndex);
		w.U8(static_cast<std::uint8_t>(target));
		w.U32(static_cast<std::uint32_t>(args.size()));
		w.Bytes(args);
		return w.Take();
	}

	std::optional<RpcMessage> DecodeRpc(ByteReader& r)
	{
		RpcMessage msg;
		msg.netId = r.U32();
		msg.scriptTypeHash = r.U32();
		msg.methodIndex = r.U16();
		const std::uint8_t target = r.U8();
		const std::uint32_t argBytes = r.U32();
		if (!r.Ok())
		{
			return std::nullopt;
		}
		if (target > kNetRpcTargetMax)
		{
			// Drop, never default. Coercing an unrecognised target to Server would
			// hand a peer a way to pick which direction gate its packet is measured
			// against just by writing a byte we do not understand.
			return std::nullopt;
		}
		msg.target = static_cast<NetRpcTarget>(target);
		// Bytes() is bounds-checked, so a hostile length yields an empty vector and
		// a failed reader rather than an over-read.
		msg.args = r.Bytes(argBytes);
		if (!r.Ok() || msg.netId == 0)
		{
			return std::nullopt;
		}
		return msg;
	}

	void ApplyRpc(World& world, NetSession& session, const RpcBridge& bridge, const RpcMessage& msg,
	        ConnectionId sender, bool localIsHost)
	{
		// Direction first: a Server call travels client-to-host and the other two
		// travel host-to-client, so each role accepts exactly one of the two
		// directions. This is what makes "only the host originates a multicast" true -
		// a client can write any target byte it likes, but the host drops every
		// inbound call that is not Server before anything else is even looked up.
		const bool inboundIsServerBound = msg.target == NetRpcTarget::Server;
		if (localIsHost != inboundIsServerBound)
		{
			return;
		}

		const Entity entity = session.EntityFor(msg.netId);
		if (!entity.IsValid())
		{
			return;
		}
		if (localIsHost)
		{
			// A client may only drive what it owns. An entity with no NetworkIdentity
			// is not a replicated entity at all, so nothing a peer says addresses it.
			const auto* identity = world.TryGet<NetworkIdentity>(entity);
			if (identity == nullptr || identity->owner != sender)
			{
				return;
			}
		}
		const auto* scripts = world.TryGet<ScriptComponent>(entity);
		if (scripts == nullptr)
		{
			return;
		}
		const std::optional<std::uint32_t> scriptIndex = FindScriptIndex(*scripts, msg.scriptTypeHash);
		if (!scriptIndex.has_value())
		{
			return; // unknown type hash: no script on this entity matches
		}
		bridge.Invoke(entity, *scriptIndex, msg.methodIndex, msg.args);
	}

	bool RpcTargetMismatch(std::int32_t expectedTarget, NetRpcTarget declared)
	{
		return expectedTarget >= 0 && expectedTarget != static_cast<std::int32_t>(declared);
	}

	RpcRoute RouteRpc(const World& world, const NetSession& session, NetRpcTarget target, Entity entity)
	{
		RpcRoute route;

		if (session.Role() == NetRole::Offline)
		{
			// No session: this process is the whole world, so every target resolves
			// to "here". Project code written for multiplayer runs unchanged.
			route.allowed = true;
			route.invokeLocally = true;
			return route;
		}

		const std::uint32_t netId = session.NetIdFor(entity);

		if (session.Role() == NetRole::Client)
		{
			if (target != NetRpcTarget::Server)
			{
				// AUTHORITY. Only the host originates a host-to-client call. A client
				// that wants everyone to hear something sends a Server RPC and lets the
				// host decide whether to multicast it - which is exactly the chat flow.
				return route;
			}
			if (netId == 0)
			{
				return route; // unreplicated entity: the call would arrive addressed to nothing
			}
			route.allowed = true;
			route.recipients.push_back(kInvalidConnection); // the transport reads this as "the host"
			return route;
		}

		// Host.
		if (target == NetRpcTarget::Server)
		{
			route.allowed = true;
			route.invokeLocally = true;
			return route;
		}

		if (netId == 0)
		{
			// A host-to-client packet names the entity by net id. Without one there is
			// nothing to address, and quietly running it here instead would report
			// success for a call no client ever saw.
			return route;
		}

		if (target == NetRpcTarget::Multicast)
		{
			route.allowed = true;
			route.invokeLocally = true; // see RouteRpc's contract: the host hears its own multicast
			const std::vector<ConnectionId>& connections = session.Connections();
			route.recipients.assign(connections.begin(), connections.end());
			return route;
		}

		// Client: the one connection that owns the entity.
		const auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity == nullptr)
		{
			return route; // not a replicated entity, so it has no owning client
		}
		route.allowed = true;
		if (identity->owner == kInvalidConnection)
		{
			// The host owns it, so the host IS the owning client. Running it here is
			// the same answer the rule gives for every other owner.
			route.invokeLocally = true;
			return route;
		}
		const std::vector<ConnectionId>& connections = session.Connections();
		if (std::find(connections.begin(), connections.end(), identity->owner) != connections.end())
		{
			route.recipients.push_back(identity->owner);
		}
		// An owner that is no longer connected leaves no recipients: the call is
		// allowed but there is nobody left to deliver it to.
		return route;
	}
}
