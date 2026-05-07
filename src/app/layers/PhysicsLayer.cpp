#include "PhysicsLayer.hpp"

#include <array>
#include <cstdio>

#include "layers/OverlayStyle.hpp"
#include "Logger.hpp"
#include "systems/PhysicsGameSystem.hpp"
#include "UiLayout.hpp"
#include "UIRenderer.hpp"
#include "World.hpp"

namespace aether::app
{
	void PhysicsLayer::OnAttach(LayerContext& context)
	{
		INFO(aether::LogCategory::App, "PhysicsLayer attached.");

		// Register the physics system first - the game system's OnRegister
		// calls AddBoxBody / AddSphereBody, so physics must already be alive.
		auto physics = std::make_unique<aether::PhysicsSystem>();
		m_physics = physics.get();
		context.world->RegisterSystem(std::move(physics));

		// Now register the game system (will call OptimizeBroadPhase internally).
		auto gameSystem = std::make_unique<PhysicsGameSystem>();
		gameSystem->Init(context.engine, *context.assets,
		                 *context.cameras, *context.input, *m_physics);
		m_gameSystem = gameSystem.get();
		context.world->RegisterSystem(std::move(gameSystem));
	}

	void PhysicsLayer::OnDetach(LayerContext& context)
	{
		// Game system must be removed before physics system so its
		// OnUnregister can still call RemoveBody.
		context.world->UnregisterSystem("PhysicsGameSystem");
		context.world->UnregisterSystem("PhysicsSystem");
		m_gameSystem = nullptr;
		m_physics    = nullptr;
	}

	void PhysicsLayer::OnUpdate([[maybe_unused]] LayerContext& context)
	{
	}

	void PhysicsLayer::OnGui(LayerContext& context)
	{
		if (!context.ui || !m_gameSystem)
			return;

		using namespace aether::app::overlay;

		aether::UIRenderer& ui = *context.ui;
		std::array<char, 64> buf{};

		PanelBuilder panel(ui, { 0.f, 0.f }, 12.f, 360.f, 12.f, 120.f);
		panel.Title("PHYSICS").Section("SIMULATION");

		std::snprintf(buf.data(), buf.size(), "%d", m_gameSystem->GetActiveBodyCount());
		panel.KV("Dynamic bodies", buf.data());

		std::snprintf(buf.data(), buf.size(), "%d", m_gameSystem->GetProjectileCount());
		panel.KV("Projectiles", buf.data());

		std::snprintf(buf.data(), buf.size(), "%.1f s", m_gameSystem->GetSimTime());
		panel.KV("Sim time", buf.data());

		std::snprintf(buf.data(), buf.size(), "%.0f Hz (fixed)", 1.f / aether::PhysicsSystem::kFixedTimestep);
		panel.KV("Step rate", buf.data(), kColorGood);

		panel.Section("CONTROLS");
		panel.KV("Space", "Fire projectile", kColorWarn);
		panel.KV("R",     "Reset scene");
		panel.KV("C",     "Toggle camera");
	}

} // namespace aether::app
