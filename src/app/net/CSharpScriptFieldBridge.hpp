#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "net/NetScriptFields.hpp"

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
	// The live ScriptFieldBridge: resolves an entity's script instance handle through
	// ScriptComponentSystem and reads/writes its properties through the same managed
	// property bridge the inspector uses. Kept in its own translation unit so the
	// packet logic in NetScriptFields.cpp never links the CoreCLR host.
	class CSharpScriptFieldBridge final : public ScriptFieldBridge
	{
	public:
		CSharpScriptFieldBridge(const aether::app::scripting::CSharpScriptingSubsystem& scripting,
		        const aether::app::ScriptComponentSystem& instances)
		      : m_scripting(scripting)
		      , m_instances(instances)
		{
		}

		[[nodiscard]] std::vector<ScriptPropertyDesc> ReplicatedProperties(const std::string& typeName) const override;
		[[nodiscard]] bool GetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        ScriptPropertyValue& out) const override;
		void SetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
		        const ScriptPropertyValue& value) const override;

		// Drops the cached per-type replicated-property tables built by ReplicatedProperties.
		// CSharpScriptingSubsystem exposes no reload signal (version counter or callback) this
		// bridge can hook itself, so nothing calls this yet - whoever wires this bridge into a
		// live NetSession must call it after CSharpScriptingSubsystem::LoadScripts() runs (the
		// single choke point every reload path - initial load, in-place reload, PlaySession -
		// funnels through), or a hot-reload that changes a script's fields keeps serving the
		// stale table.
		void ClearCache() const
		{
			m_propertyCache.clear();
		}

	private:
		const aether::app::scripting::CSharpScriptingSubsystem& m_scripting;
		const aether::app::ScriptComponentSystem& m_instances;

		// Per-type replicated-property descriptors, built once per type on first request.
		// ReplicatedProperties() is called once per entity per script per tick from the
		// hot net path (BuildScriptFieldPacket/ApplyScriptFieldPacket); without this the
		// bridge would re-marshal the whole property table across the CLR boundary every
		// single call even though the managed side already caches it once (s_props).
		mutable std::unordered_map<std::string, std::vector<ScriptPropertyDesc>> m_propertyCache;
	};
} // namespace aether::net
