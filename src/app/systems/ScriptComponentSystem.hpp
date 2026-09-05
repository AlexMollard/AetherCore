#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "net/NetTypes.hpp"
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

namespace aether::scripting
{
	struct ManagedScriptApi;
} // namespace aether::scripting

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

		// Scene.Load support: destroys instances whose entities die in the switch
		// while keeping persistent (SceneTransient) entities' instances alive.
		void PruneInstancesForSceneSwitch(World& world, scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx);

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

			// Ownership-changed dispatch state - see DispatchOwnershipChanged. `known`
			// is false until the first frame this instance's ownership is decidable
			// (immediately offline/on the host/unreplicated; only a welcomed client
			// waits). `settled` is set once `known` is true AND the entity carries no
			// NetworkIdentity: an unreplicated entity's ownership can never change
			// again, so DispatchOwnershipChanged retires the check permanently instead
			// of paying a lookup for it every frame for the rest of its life - the
			// same "a hook nobody uses costs nothing ongoing" property
			// DispatchPhysicsEvents gives EntityScript, reached by a different route
			// because this hook is pushed by native rather than polled by managed.
			bool known = false;
			bool settled = false;
			bool isOwner = false;
			net::ConnectionId owner = net::kInvalidConnection;
		};

		// Detects whether `instance`'s entity's ownership just became known or just
		// changed hands and, if so, calls the appended
		// ManagedScriptApi::InvokeOwnershipChanged slot - see the .cpp for why this
		// lives inline in the per-instance update rather than as its own system pass.
		void DispatchOwnershipChanged(World& world, Entity entity, const ::aether::scripting::ManagedScriptApi& api, std::uint64_t handle, Instance& instance);

		std::unordered_map<std::uint64_t, Instance> m_instances;
		std::unordered_set<std::string> m_failedTypes;
	};
} // namespace aether::app
