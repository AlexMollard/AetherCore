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

	// Where a [NetRpc] method executes. The C++ mirror of NetRpcTarget in
	// managed/AetherCore/NetAttributes.cs - the two must agree numerically, because
	// the value crosses the interop boundary as a raw integer and then goes on the
	// wire as a raw byte.
	//
	// The target travels WITH the call rather than being inferred from the receiving
	// role, so the receiver can check direction (ApplyRpc). Without it a client could
	// send a packet the host would happily treat as a host-originated multicast, and
	// a compromised or buggy host could push a Server-target call at a client. Each
	// role accepts exactly one direction and drops the other.
	enum class NetRpcTarget : std::uint8_t
	{
		// Called on a client, executed on the host. Also the only target that runs
		// locally on a host or in an unnetworked game.
		Server = 0,
		// Called on the host, executed on the owning client of the target entity.
		Client = 1,
		// Called on the host, executed on every connected client AND on the host.
		Multicast = 2,
	};

	// The largest value the enum defines. DecodeRpc rejects anything above it rather
	// than coercing it to Server - a target byte we do not recognise is a packet we
	// cannot reason about the direction of, which is precisely what must not be
	// guessed at.
	inline constexpr std::uint8_t kNetRpcTargetMax = static_cast<std::uint8_t>(NetRpcTarget::Multicast);

	struct RpcMessage
	{
		std::uint32_t netId = 0;
		std::uint32_t scriptTypeHash = 0;
		// The method travels by NAME, never by table index: two peers built from
		// different source (a hot reload on one side, a client on an older build)
		// have different declaration-order tables, so the same index names a
		// DIFFERENT method on each. A name either resolves against the receiver's
		// own table or the call is dropped - it cannot silently land on the wrong
		// method.
		std::string methodName;
		NetRpcTarget target = NetRpcTarget::Server;
		std::vector<std::byte> args;
	};

	[[nodiscard]] std::vector<std::byte> EncodeRpc(std::uint32_t netId, std::uint32_t scriptTypeHash,
	        std::string_view methodName, NetRpcTarget target, std::span<const std::byte> args);
	[[nodiscard]] std::optional<RpcMessage> DecodeRpc(ByteReader& r);

	// ScriptTypeHash itself lives in NetScriptFields.hpp (Task 9) and is reused here
	// unchanged - see that header for the FNV-1a definition. A second definition in
	// this header would be a silent divergence risk the moment either one changes.

	// A [NetRpc] method resolved by name: the index the RESOLVING peer's own
	// table assigns it (used for local dispatch only - the wire carries the name)
	// and the target its attribute declared.
	struct RpcMethod
	{
		int index = -1; // < 0 = the type is unknown or declares no such RPC
		NetRpcTarget target = NetRpcTarget::Server;

		[[nodiscard]] bool Found() const
		{
			return index >= 0;
		}
	};

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

		// Resolves `methodName` in `typeName`'s [NetRpc] method table. Used by BOTH
		// sides of the wire: an outbound call turns the script's method name into
		// the local index it dispatches at (via NetworkContext::Rpcs, the cached
		// bridge, rather than a per-call temporary), and ApplyRpc turns the name
		// that ARRIVED into this peer's index plus the declaration the wire target
		// is checked against.
		//
		// The target comes from the declaration rather than from the call site on
		// purpose: [NetRpc(...)] is the single place a method's direction is stated,
		// so a call site - or a wire packet - cannot disagree with it. See Net.Call
		// in Net.cs and ApplyRpc's declaration gate.
		[[nodiscard]] virtual RpcMethod FindMethod(const std::string& typeName, const std::string& methodName) const = 0;

		// Invokes RPC method `methodIndex` on the live instance of
		// ScriptComponent::scripts[scriptIndex] on `entity`. A silent no-op when
		// there is no live instance (not attached, or edit mode).
		virtual void Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex,
		        std::span<const std::byte> args) const = 0;
	};

	// Host or client. Resolves msg.netId to an entity via `session` and
	// msg.scriptTypeHash to a script index on that entity's ScriptComponent (the
	// same match NetScriptFields uses), then resolves msg.methodName through
	// `bridge`.FindMethod against THAT script's table and invokes the local index
	// it names. Drops the call silently - a peer can name anything - when the net
	// id is unknown, the entity carries no ScriptComponent, no script on it hashes
	// to scriptTypeHash, or the method name resolves to no [NetRpc] method this
	// peer knows (a build-skewed peer drops rather than running whatever its table
	// happens to hold).
	//
	// DIRECTION GATE, checked first. A host accepts ONLY NetRpcTarget::Server and a
	// client accepts ONLY Client/Multicast, because those are the only directions
	// that exist: Server travels client-to-host, the other two host-to-client. The
	// gate is what stops a client originating a multicast - it can put any target
	// byte it likes in a packet, but the host drops everything that is not Server, so
	// the authoritative broadcast can only ever start on the host.
	//
	// OWNERSHIP GATE. RPC is the ONLY channel by which a client can affect host state
	// (every other inbound message is dropped by a role guard), so on the host the
	// call is additionally rejected unless the target entity carries a
	// NetworkIdentity whose `owner` is `sender`. Without it any connected client
	// could invoke any [NetRpc] method on any replicated entity - another player's
	// TakeDamage, Respawn, whatever the project marks up. `localIsHost` selects the
	// gate: a client applying a host-sent call is not owner-checked (the host is
	// authoritative over everything), and `sender` is ignored there.
	//
	// DECLARATION GATE, checked last because it needs the bridge's table. The wire
	// target byte is a claim the sender wrote; the [NetRpc] attribute is the only
	// statement of where a method may run. The call is dropped unless the target the
	// resolved method DECLARED equals the target on the wire. Direction and ownership
	// both pass for an owning client sending target=Server at a [NetRpc(Client)] or
	// [NetRpc(Multicast)] method - only the declaration contradicts that packet, so
	// without this gate the client executes a host-side method body on the host.
	//
	// Every drop above also leaves exactly one actionable, deduplicated AE_WARN
	// behind (LogCategory::App) - see NetRpc.cpp's WarnRpcDropOnce - naming the
	// symbol involved and, for the ownership gate specifically, the fix ("send it
	// on an entity you own"). Warning is keyed per distinct cause for the life of
	// the process, so a flood of bad calls cannot flood the log; nothing here
	// changes which calls are accepted or dropped.
	void ApplyRpc(World& world, NetSession& session, const RpcBridge& bridge, const RpcMessage& msg,
	        ConnectionId sender, bool localIsHost);

	// ── Outbound ────────────────────────────────────────────────────────────────

	// Why RouteRpc refused a call, kept purely for diagnostics: NetRpcExports.cpp
	// reads this to tell a script author what to fix when Net.Call returns false.
	// Nothing reads `reason` to decide anything - `allowed` alone is the decision
	// RouteRpc has always made, so adding or renaming a case here can never change
	// behaviour.
	enum class RpcRouteRefusal : std::uint8_t
	{
		None,                      // route.allowed - nothing to explain
		ClientOriginatedBroadcast, // a client tried to send Client or Multicast
		Unreplicated,              // the target entity has no net id
		MissingNetworkIdentity,    // has a net id but no NetworkIdentity component
	};

	// Where one outbound call goes. Returned rather than sent so the whole authority
	// decision is a pure function of role, target and ownership, and can be tested
	// without a live transport - the actual send is then a loop with no policy in it
	// (NetRpcExports.cpp).
	struct RpcRoute
	{
		// False = refuse the call outright. The caller reports failure to the script
		// rather than falling back to a local invoke: an RPC that silently becomes a
		// local call runs against unreplicated state and reports success, which is
		// strictly worse than a dropped one.
		bool allowed = false;
		// Run it on this peer as well as (or instead of) sending it.
		bool invokeLocally = false;
		// Peers to send the encoded packet to. On a client the single entry is
		// kInvalidConnection, which the transport reads as "the host".
		std::vector<ConnectionId> recipients;
		// See RpcRouteRefusal. Meaningless when `allowed` is true.
		RpcRouteRefusal reason = RpcRouteRefusal::None;
	};

	// Whether an explicit call-site target contradicts the method's [NetRpc]
	// declaration. `expectedTarget` is the raw integer Net.CallServer sends across
	// interop: negative means "whatever the method declares" (Net.Call), so only a
	// non-negative value that differs from the declaration is a mismatch, and a
	// mismatch is refused rather than re-routed - the call site and the declaration
	// disagree, and silently picking a winner would make one of the two a lie.
	//
	// A predicate rather than an inline comparison in NetRpcExports.cpp because that
	// TU is CLR-linked and EngineTests cannot link it, so the refusal it guards had no
	// way of being tested where it lived. RouteRpc never sees `expectedTarget` either,
	// so nothing else covered it.
	[[nodiscard]] bool RpcTargetMismatch(std::int32_t expectedTarget, NetRpcTarget declared);

	// The delivery plan for calling `target` on `entity` from THIS peer.
	//
	//   Offline           any target -> local invoke. A single-player build runs
	//                     every RPC in-process, so project code needs no role test.
	//   Client, Server    -> the host. Requires a net id: an unbound entity has no
	//                     name on the wire, so the call would arrive addressed to
	//                     nothing.
	//   Client, Client/Multicast
	//                     -> REFUSED. The host is authoritative; only it originates
	//                     host-to-client calls. This is the send-side half of the
	//                     direction gate in ApplyRpc.
	//   Host, Server      -> local invoke. The host IS the server.
	//   Host, Client      -> the one connection that owns `entity`. An entity the
	//                     host itself owns has no remote owning client, so the call
	//                     runs locally instead.
	//   Host, Multicast   -> every connection AND locally. Running it on the host too
	//                     is the UE5 behaviour and what a caller wants for the
	//                     motivating case (a chat line the host must also see).
	//
	// Host-to-client targets require `entity` to be replicated (a live net id); a
	// packet naming net id 0 addresses nothing on the far end.
	[[nodiscard]] RpcRoute RouteRpc(const World& world, const NetSession& session, NetRpcTarget target, Entity entity);

	// Test-only: clears ApplyRpc's once-per-cause warning dedup. Without this a
	// TEST_CASE that intentionally repeats a bad call to prove "warns once" would
	// see zero warnings whenever an earlier TEST_CASE already tripped the same
	// cause - the dedup state is a process-wide static, same as it would be in a
	// real running host. Never call this outside tests.
	void ResetRpcApplyWarningsForTest();
} // namespace aether::net
