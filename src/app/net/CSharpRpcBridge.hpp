#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "net/NetRpc.hpp"
#include "scene/Entity.hpp"

namespace aether::app
{
	class ScriptComponentSystem;
}

namespace aether::app::scripting
{
	class CSharpScriptingSubsystem;
}

namespace aether::net
{
	// The live counterpart of EncodeRpc/DecodeRpc: resolves a script's [NetRpc]
	// method table through the managed registry and invokes it through the same
	// instance-handle bridge InvokeAttach/InvokeUpdate use. Kept in its own
	// translation unit so NetRpc.cpp - the pure encode/decode logic under test in
	// EngineTests - never links the CoreCLR host, exactly as NetScriptFields.cpp is
	// split from CSharpScriptFieldBridge.cpp.
	class CSharpRpcBridge final : public RpcBridge
	{
	public:
		CSharpRpcBridge(const aether::app::scripting::CSharpScriptingSubsystem& scripting,
		        const aether::app::ScriptComponentSystem& instances)
		      : m_scripting(scripting)
		      , m_instances(instances)
		{
		}

		// `methodName`'s wire index in `typeName`'s [NetRpc] method table plus the
		// target it declared, or index -1 if the type is unknown or declares no such
		// RPC. Used by the encode side (a caller building an outbound call, e.g.
		// Net.Call's native half) to turn a method name into what goes on the wire.
		[[nodiscard]] RpcMethod FindMethod(const std::string& typeName, const std::string& methodName) const override;

		// Invokes RPC method `methodIndex` on the live instance of
		// ScriptComponent::scripts[scriptIndex] on `entity`. A silent no-op when
		// there is no live instance (not attached, or edit mode) - a peer, or a
		// stale local caller, can name an entity or script that no longer exists.
		void Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex,
		        std::span<const std::byte> args) const override;

	private:
		const aether::app::scripting::CSharpScriptingSubsystem& m_scripting;
		const aether::app::ScriptComponentSystem& m_instances;
	};
} // namespace aether::net
