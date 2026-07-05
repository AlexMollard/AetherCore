#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "scene/System.hpp"
#include "scripting/ScriptHandle.hpp"

namespace aether
{
	class ServiceContainer;
} // namespace aether

namespace aether::app
{
	// Drives ScriptComponent entities: compiles each unique script path once,
	// calls on_entity_attach(world, self) the first tick an entity is seen and
	// on_entity_update(world, self, dt) every tick after. Registered with the
	// World's systems, so the editor's play gate freezes entity scripts with
	// physics/animation/behaviors - and because scene apply resets the
	// component's `attached` flag, loads and Stop-restores re-run attach on
	// the next play tick with no extra bookkeeping.
	//
	// das today; when the C# port lands only this runner changes - the
	// component, records and editor UI stay.
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

		// F5 support: frees every compiled handle, forgets failures, and marks
		// all live components detached so the next play tick recompiles and
		// re-attaches from fresh sources.
		void Invalidate(World& world);

	private:
		scripting::ScriptHandle* HandleFor(const std::string& path);

		ServiceContainer& m_services;
		std::unordered_map<std::string, scripting::ScriptHandle> m_handles;
		// Paths that failed to compile: skipped until Invalidate, so a broken
		// script logs once instead of recompiling every frame.
		std::unordered_set<std::string> m_failed;
	};
} // namespace aether::app
