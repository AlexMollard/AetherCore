#include "net/CSharpRpcBridge.hpp"

#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/ServiceContainer.hpp"

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

		// An RPC body is ordinary script code: it calls Net.*, Entity.*, Ui.* like any
		// other callback, and every one of those exports dereferences the active scene
		// context. An INBOUND call arrives on the network system's tick, where nothing
		// has published one - so without this scope the first engine call the method
		// makes takes the process down (an access violation inside the P/Invoke, with
		// the managed stack as the only clue). The locally-routed case already runs
		// inside a script update; the scope restores rather than clears, so nesting
		// there is harmless.
		auto* sceneCtx = m_services.TryGet<aether::app::scripting::SceneContext>();
		if (sceneCtx == nullptr)
		{
			// No scene context registered at all (a headless/tools build). Better to
			// drop the call than to invoke script code that cannot reach the engine.
			return;
		}
		const aether::app::scripting::ActiveContextScope scope(*sceneCtx);
		m_scripting.InvokeNetRpc(handle, static_cast<int>(methodIndex), args);
	}
} // namespace aether::net
