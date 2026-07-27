#pragma once

#include <string>
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

	private:
		const aether::app::scripting::CSharpScriptingSubsystem& m_scripting;
		const aether::app::ScriptComponentSystem& m_instances;
	};
} // namespace aether::net
