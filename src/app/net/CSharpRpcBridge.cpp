#include "net/CSharpRpcBridge.hpp"

#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::net
{
	RpcMethod CSharpRpcBridge::FindMethod(const std::string& typeName, const std::string& methodName) const
	{
		int target = 0;
		const int index = m_scripting.FindNetRpcMethod(typeName, methodName, target);
		if (index < 0 || target < 0 || target > static_cast<int>(kNetRpcTargetMax))
		{
			// A target the registry reports that this build does not know is treated
			// as no method at all: the alternative is guessing a direction for it.
			return RpcMethod{};
		}
		return RpcMethod{.index = index, .target = static_cast<NetRpcTarget>(target)};
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
