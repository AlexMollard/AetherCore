#include "scripting/interop/InteropCommon.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

namespace
{
	// RPC dispatch is synchronous: a Server call that routes locally invokes the
	// method body on the spot, and a body that Net.Calls another Server method
	// re-enters this export on the same native stack. Nothing else on the path
	// bounds that, and the failure mode is not recoverable - a stack overflow
	// inside the P/Invoke kills the process, because StackOverflowException is
	// uncatchable and the managed try/catch in ScriptRegistry.InvokeNetRpc never
	// runs. The export itself is therefore where the recursion must stop.
	//
	// thread_local rather than a NetworkContext member: the native -> managed ->
	// native re-entry chain runs on one thread, so per-thread is exactly the
	// nesting being measured, and a context-wide counter would couple unrelated
	// dispatch threads for no gain.
	//
	// ponytail: a depth cap, not a cycle detector - mutually recursive RPCs are
	// legal below the cap; raise it if a project legitimately needs more nesting.
	constexpr int kMaxRpcDepth = 8;

	thread_local int t_rpcDepth = 0;

	// Counts one nested Net.Call for the duration of the scope, on every exit
	// path including the exception unwind SafeExport catches above us.
	struct RpcDepthGuard
	{
		bool ok;

		RpcDepthGuard()
		  : ok(t_rpcDepth < kMaxRpcDepth)
		{
			++t_rpcDepth;
		}

		~RpcDepthGuard()
		{
			--t_rpcDepth;
		}

		RpcDepthGuard(const RpcDepthGuard&) = delete;
		RpcDepthGuard& operator=(const RpcDepthGuard&) = delete;
	};

	// Every distinct reason Net.Call's native half refuses an outbound call.
	// Separate from ApplyRpc's cause list (NetRpc.cpp): these diagnose "why did my
	// Net.Call return false" on the process that MADE the call, not on whichever
	// peer might have received it.
	enum class RpcCallDropCause : std::uint8_t
	{
		NoScriptComponent,
		MethodNotDeclared,
		TargetMismatch,
		IndexOverflow,
		RouteRefused,
		Count,
	};

	// One flag per cause, same reasoning as ApplyRpc's dedup in NetRpc.cpp: bounds
	// this export's own logging to at most Count lines for the life of the
	// process no matter how a misbehaving or buggy caller floods Net.Call.
	std::atomic_flag g_callDropWarned[static_cast<std::size_t>(RpcCallDropCause::Count)];

	// A method name is caller-controlled and unbounded; capped so a pathological
	// name cannot make the log line itself the payload.
	constexpr std::size_t kLoggedNameCap = 96;

	std::string CapName(std::string_view name)
	{
		if (name.size() <= kLoggedNameCap)
		{
			return std::string(name);
		}
		return std::string(name.substr(0, kLoggedNameCap)) + "...(truncated)";
	}

	// Logs `fmt` at most once per `cause` for the life of the process - see
	// ApplyRpc's WarnRpcDropOnce (NetRpc.cpp) for the identical reasoning. Free on
	// every path except a repeat refusal, which is now bounded rather than spammy.
	template<typename... Args>
	void WarnCallDropOnce(RpcCallDropCause cause, std::format_string<Args...> fmt, Args&&... args)
	{
		if (g_callDropWarned[static_cast<std::size_t>(cause)].test_and_set(std::memory_order_relaxed))
		{
			return;
		}
		AE_WARN(aether::LogCategory::App, fmt, std::forward<Args>(args)...);
	}
} // namespace

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
	// SafeExport so no C++ exception (bad_alloc, a throwing Send) unwinds out of
	// the [LibraryImport] stub: the CLR cannot unwind native frames, and what
	// should be a logged script error would FailFast the process instead - the
	// same failure class that already took the editor down once from a json
	// exception escaping a native callback.
	return SafeExport([&]() -> std::int32_t
	{
		const RpcDepthGuard depth;
		if (!depth.ok)
		{
			AE_WARN(aether::LogCategory::App, "Net.Call exceeded the max RPC recursion depth {}; dropping the call",
			        kMaxRpcDepth);
			return 0;
		}
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

		const aether::Entity entity{entityId};
		// Id 0 is the SDK's invalid handle, but the underlying slot 0 can hold a
		// live entity - resolving RPCs against it would run some unrelated
		// script's method table for a caller that passed default(Entity).
		if (!entity.IsValid())
		{
			return 0;
		}
		aether::World& world = ActiveWorld();
		const auto* scripts = world.TryGet<aether::ScriptComponent>(entity);
		if (scripts == nullptr)
		{
			WarnCallDropOnce(RpcCallDropCause::NoScriptComponent,
			        "Net.Call refused: entity {} has no ScriptComponent, so it declares no [NetRpc] methods at "
			        "all; '{}' cannot run.",
			        entityId, CapName(methodNameUtf8));
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
				WarnCallDropOnce(RpcCallDropCause::TargetMismatch,
				        "Net.Call refused: '{}' on entity {} declares [NetRpc] target={}, but was called with an "
				        "explicit target of {}; call it through Net.Call (whatever it declares), or fix the "
				        "explicit target to match the attribute.",
				        CapName(methodName), entityId, static_cast<int>(method.target), expectedTarget);
				return 0;
			}
			if (method.index > 0xFFFF)
			{
				// The dispatch index the local invoke hands to bridge.Invoke is u16; an
				// index above it would wrap to a DIFFERENT method of this peer's own
				// table, and nothing downstream could tell. Refuse the call.
				WarnCallDropOnce(RpcCallDropCause::IndexOverflow,
				        "Net.Call refused: '{}' on entity {} resolved to local dispatch index {}, which exceeds "
				        "the 65535 limit the wire format allows.",
				        CapName(methodName), entityId, method.index);
				return 0;
			}

			const aether::net::RpcRoute route = aether::net::RouteRpc(world, session, method.target, entity);
			if (!route.allowed)
			{
				std::string_view why = "refused by routing policy";
				switch (route.reason)
				{
					case aether::net::RpcRouteRefusal::ClientOriginatedBroadcast:
						why = "a client cannot originate a Client/Multicast RPC - declare it [NetRpc(Server)] and "
						      "let the host decide, or only invoke Client/Multicast methods from the host";
						break;
					case aether::net::RpcRouteRefusal::Unreplicated:
						why = "the target entity has no net id (it is not replicated) - give it a NetworkIdentity "
						      "before calling networked RPCs on it";
						break;
					case aether::net::RpcRouteRefusal::MissingNetworkIdentity:
						why = "the target entity has a net id but no NetworkIdentity component";
						break;
					case aether::net::RpcRouteRefusal::None:
					default:
						break;
				}
				WarnCallDropOnce(RpcCallDropCause::RouteRefused, "Net.Call refused: '{}' on entity {}: {}.",
				        CapName(methodName), entityId, why);
				return 0;
			}

			if (!route.recipients.empty() && network != nullptr)
			{
				// The method travels by NAME: the receiver resolves it against its own
				// [NetRpc] table (ApplyRpc), so a peer whose script assembly differs - a
				// hot reload on one side, a client on an older build - finds no such
				// method and drops the call rather than invoking whatever happens to sit
				// at some table index. Encoded once and sent to each recipient - a
				// multicast to eight connections is one buffer, not eight.
				const std::vector<std::byte> packet = aether::net::EncodeRpc(session.NetIdFor(entity),
				        aether::net::ScriptTypeHash(typeName), methodName, method.target, args);
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
		WarnCallDropOnce(RpcCallDropCause::MethodNotDeclared,
		        "Net.Call refused: no [NetRpc] method named '{}' is declared on any of the {} script(s) attached "
		        "to entity {}; attach a script whose [NetRpc] method has that exact name, or check for a typo.",
		        CapName(methodName), scripts->scripts.size(), entityId);
		return 0; // no script on this entity declares that RPC
	});
}
