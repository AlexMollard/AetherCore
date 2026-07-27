#pragma once

#include <cstdint>
#include <optional>
#include <span>
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
	void ApplyRpc(World& world, NetSession& session, const RpcBridge& bridge, const RpcMessage& msg);
} // namespace aether::net
