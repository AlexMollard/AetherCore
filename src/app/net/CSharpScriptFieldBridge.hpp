#pragma once

#include <cstdint>
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
		// ReplicatedProperties() calls this itself whenever CSharpScriptingSubsystem's reload
		// generation moves, which covers every reload path (initial load, in-place reload,
		// project open, PlaySession) because they all funnel through LoadScripts(). It stays
		// public so a caller that reloads scripts by some future route can still force it.
		void ClearCache() const
		{
			m_propertyCache.clear();
		}

	private:
		// Drops the cache if scripts reloaded since it was built. Hooking the reload
		// this way rather than calling ClearCache() from each LoadScripts() call site is
		// deliberate: a new call site cannot forget to invalidate a cache that checks
		// for itself, and a stale table silently replicates the wrong property.
		void SyncToScriptReload() const;

		const aether::app::scripting::CSharpScriptingSubsystem& m_scripting;
		const aether::app::ScriptComponentSystem& m_instances;

		// Reload generation the cached tables were built against.
		mutable std::uint32_t m_cacheGeneration = 0;
		mutable bool m_cacheSeeded = false;

		// Per-type replicated-property descriptors, built once per type on first request.
		// ReplicatedProperties() is called once per entity per script per tick from the
		// hot net path (BuildScriptFieldPacket/ApplyScriptFieldPacket); without this the
		// bridge would re-marshal the whole property table across the CLR boundary every
		// single call even though the managed side already caches it once (s_props).
		mutable std::unordered_map<std::string, std::vector<ScriptPropertyDesc>> m_propertyCache;
	};
} // namespace aether::net
