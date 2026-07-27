#include "net/CSharpRpcBridge.hpp"

#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::net
{
	int CSharpRpcBridge::FindMethodIndex(const std::string& typeName, const std::string& methodName) const
	{
		return m_scripting.FindNetRpcMethodIndex(typeName, methodName);
	}

	void CSharpRpcBridge::Invoke(Entity entity, std::uint32_t scriptIndex, std::uint16_t methodIndex,
	        std::span<const std::byte> args) const
	{
		const std::uint64_t handle = m_instances.GetInstanceHandle(entity.id, scriptIndex);
		if (handle == 0)
		{
			return; // no live instance yet (not attached, or edit mode)
		}
		m_scripting.InvokeNetRpc(handle, static_cast<int>(methodIndex), args);
	}
} // namespace aether::net
