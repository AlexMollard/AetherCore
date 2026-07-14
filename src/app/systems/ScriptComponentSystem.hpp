#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/System.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether::app::scripting
{
	struct SceneContext;
	class CSharpScriptingSubsystem;
} // namespace aether::app::scripting

namespace aether::app
{
	class ScriptComponentSystem final : public System
	{
	public:
		explicit ScriptComponentSystem(ServiceContainer& services)
		      : m_services(services)
		{
		}

		~ScriptComponentSystem() override;
		ScriptComponentSystem(const ScriptComponentSystem&) = delete;
		ScriptComponentSystem& operator=(const ScriptComponentSystem&) = delete;
		ScriptComponentSystem(ScriptComponentSystem&&) = delete;
		ScriptComponentSystem& operator=(ScriptComponentSystem&&) = delete;

		[[nodiscard]] const char* GetName() const override
		{
			return "ScriptComponentSystem";
		}

		void Update(World& world, float dt) override;

		void Invalidate(World& world);

		[[nodiscard]] std::uint64_t GetInstanceHandle(std::uint32_t entityId, std::uint32_t scriptIndex) const;

	private:
		bool UpdateCSharpEntity(scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx, Entity entity, std::uint32_t scriptIndex, ScriptEntry& script, float dt);
		void PurgeStaleCSharpInstances(World& world, scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx);
		void DestroyAllCSharpInstances(scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx);

		ServiceContainer& m_services;

		struct Instance
		{
			std::uint64_t handle = 0;
			std::string typeName;
		};

		std::unordered_map<std::uint64_t, Instance> m_instances;
		std::unordered_set<std::string> m_failedTypes;
	};
} // namespace aether::app
