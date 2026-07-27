#include "net/CSharpScriptFieldBridge.hpp"

#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::net
{
	std::vector<ScriptPropertyDesc> CSharpScriptFieldBridge::ReplicatedProperties(const std::string& typeName) const
	{
		const std::vector<int> indices = m_scripting.GetReplicatedPropertyIndices(typeName);
		if (indices.empty())
		{
			return {};
		}

		// The managed side reports indices only; the type comes from the same property
		// table the inspector reads, so there is one description of a script property.
		const std::vector<aether::app::scripting::ScriptPropertyInfo> props = m_scripting.GetScriptProperties(typeName);
		std::vector<ScriptPropertyDesc> out;
		out.reserve(indices.size());
		for (const int index: indices)
		{
			if (index < 0 || static_cast<std::size_t>(index) >= props.size())
			{
				continue; // the tables disagree - a reload mid-query; drop the field
			}
			out.push_back(ScriptPropertyDesc{
			        .index = static_cast<std::uint16_t>(index),
			        .type = props[static_cast<std::size_t>(index)].type,
			});
		}
		return out;
	}

	bool CSharpScriptFieldBridge::GetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
	        ScriptPropertyValue& out) const
	{
		const std::uint64_t handle = m_instances.GetInstanceHandle(entity.id, scriptIndex);
		if (handle == 0)
		{
			return false; // no live instance yet (not attached, or edit mode)
		}
		return m_scripting.GetPropertyValue(handle, static_cast<int>(propertyIndex), out);
	}

	void CSharpScriptFieldBridge::SetProperty(Entity entity, std::uint32_t scriptIndex, std::uint16_t propertyIndex,
	        const ScriptPropertyValue& value) const
	{
		const std::uint64_t handle = m_instances.GetInstanceHandle(entity.id, scriptIndex);
		if (handle == 0)
		{
			return;
		}
		m_scripting.SetPropertyValue(handle, static_cast<int>(propertyIndex), value);
	}
} // namespace aether::net
