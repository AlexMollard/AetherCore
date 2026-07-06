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
} // namespace aether

namespace aether::app::scripting
{
	struct SceneContext;
	class CSharpScriptingSubsystem;
} // namespace aether::app::scripting

namespace aether::app
{
	// Drives ScriptComponent entities: attaches the first tick an entity is seen
	// while playing and updates every tick after. Registered with the World's
	// systems, so the editor's play gate freezes entity scripts with
	// physics/animation/behaviors - and because scene apply resets the
	// component's `attached` flag, loads and Stop-restores re-run attach on the
	// next play tick with no extra bookkeeping.
	//
	// A ScriptComponent's `path` is a C# script type name run through
	// CSharpScriptingSubsystem; behavior lives in the AetherGame assembly.
	class ScriptComponentSystem final : public System
	{
	public:
		explicit ScriptComponentSystem(ServiceContainer& services)
		      : m_services(services)
		{
		}

		~ScriptComponentSystem() override;

		[[nodiscard]] const char* GetName() const override
		{
			return "ScriptComponentSystem";
		}

		void Update(World& world, float dt) override;

		// F5 support: frees every compiled handle / managed instance, forgets
		// failures, and marks all live components detached so the next play tick
		// recompiles and re-attaches from fresh sources.
		void Invalidate(World& world);

		// The live managed instance handle for an entity (0 if none). Used by the
		// inspector to read/write script properties on the running instance.
		[[nodiscard]] std::uint64_t GetInstanceHandle(std::uint32_t entityId) const;

	private:
		// Attaches/updates one C# entity; the caller has installed the active
		// SceneContext. Returns false if the type could not be instantiated.
		bool UpdateCSharpEntity(scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx, Entity entity,
			const std::string& typeName, const std::map<std::string, ScriptPropertyValue>& properties, bool& attached, float dt);
		// Detach + free every managed instance whose entity is no longer scripted.
		void PurgeStaleCSharpInstances(World& world, scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx);
		void DestroyAllCSharpInstances(scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx);

		ServiceContainer& m_services;

		// C# per-entity script instances: entity id -> managed GCHandle (u64).
		std::unordered_map<std::uint32_t, std::uint64_t> m_instances;
		// C# type names that failed to instantiate: skipped until Invalidate.
		std::unordered_set<std::string> m_failedTypes;
	};
} // namespace aether::app
