#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/NetScriptFields.hpp" // ScriptTypeHash - reused rather than redefined, see NetRpc.cpp
#include "net/NetSerialize.hpp"
#include "net/NetSpawn.hpp" // NetMessage
#include "scene/Entity.hpp"

namespace aether
{
	class World;
}

namespace aether::net
{
	class NetSession;

	struct RpcMessage
	{
		std::uint32_t netId = 0;
		std::uint32_t scriptTypeHash = 0;
		std::uint16_t methodIndex = 0;
		std::vector<std::byte> args;
	};

	[[nodiscard]] std::vector<std::byte> EncodeRpc(std::uint32_t netId, std::uint32_t scriptTypeHash,
	        std::uint16_t methodIndex, std::span<const std::byte> args);
	[[nodiscard]] std::optional<RpcMessage> DecodeRpc(ByteReader& r);

	// ScriptTypeHash itself lives in NetScriptFields.hpp (Task 9) and is reused here
	// unchanged - see that header for the FNV-1a definition. A second definition in
	// this header would be a silent divergence risk the moment either one changes.

	// The managed dispatch, reduced to what ApplyRpc needs - the RPC counterpart of
	// ScriptFieldBridge (NetScriptFields.hpp). Kept abstract so the resolution logic
	// below is testable headless; CSharpRpcBridge is the concrete, CLR-facing
	// implementation.
	class RpcBridge
	{
	public:
		RpcBridge() = default;
		virtual ~RpcBridge() = default;
		RpcBridge(const RpcBridge&) = delete;
		RpcBridge& operator=(const RpcBridge&) = delete;
		RpcBridge(RpcBridge&&) = delete;
		RpcBridge& operator=(RpcBridge&&) = delete;

		// Index of `methodName` in `typeName`'s [NetRpc] method table, or -1 if the
		// type is unknown or declares no such RPC. The encode side: turns a method
		// name into the index that goes on the wire. Part of the interface so a
		// caller building an outbound call can use the cached bridge
		// (NetworkContext::Rpcs) instead of constructing a concrete one.
		[[nodiscard]] virtual int FindMethodIndex(const std::string& typeName, const std::string& methodName) const = 0;

		// Invokes RPC method `methodIndex` on the live instance of
		// ScriptComponent::scripts[scriptIndex] on `entity`. A silent no-op when
		// there is no live instance (not attached, or edit mode).
		virtual void Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex,
		        std::span<const std::byte> args) const = 0;
	};

	// Host or client. Resolves msg.netId to an entity via `session` and
	// msg.scriptTypeHash to a script index on that entity's ScriptComponent (the
	// same match NetScriptFields uses), then invokes msg.methodIndex there through
	// `bridge`. Drops the call silently - a peer can name anything - when the net id
	// is unknown, the entity carries no ScriptComponent, or no script on it hashes
	// to scriptTypeHash. An out-of-range methodIndex is bounds-checked on the other
	// side of `bridge` (the managed dispatch), since only the CLR side can see a
	// script assembly that reloaded with a shorter [NetRpc] table.
	//
	// OWNERSHIP GATE. RPC is the ONLY channel by which a client can affect host state
	// (every other inbound message is dropped by a role guard), so on the host the
	// call is additionally rejected unless the target entity carries a
	// NetworkIdentity whose `owner` is `sender`. Without it any connected client
	// could invoke any [NetRpc] method on any replicated entity - another player's
	// TakeDamage, Respawn, whatever the project marks up. `localIsHost` selects the
	// gate: a client applying a host-sent call is not owner-checked (the host is
	// authoritative over everything), and `sender` is ignored there.
	void ApplyRpc(World& world, NetSession& session, const RpcBridge& bridge, const RpcMessage& msg,
	        ConnectionId sender, bool localIsHost);
} // namespace aether::net
