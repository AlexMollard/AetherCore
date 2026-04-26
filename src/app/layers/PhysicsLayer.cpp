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

		// Register the physics system first — the game system's OnRegister
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

		constexpr glm::vec2 kAnchor{ 0.f, 0.f };
		constexpr float kL   = 12.f;
		constexpr float kR   = 360.f;
		constexpr float kTop = 12.f;
		constexpr float kBot = 256.f;
		constexpr float kIL  = kL + kPad;
		constexpr float kIR  = kR - kPad;
		constexpr float kKey = kIL;
		constexpr float kVal = kIL + 120.f;

		DrawPanel(ui, kAnchor, kL, kR, kTop, kBot);

		// Title
		ui.DrawText("PHYSICS",
			aether::UiPoint{ .anchor = kAnchor, .offsetPx = { kIL, kTop + 26.f } },
			18.f, kColorTitle);

		DrawSeparator(ui, kAnchor, kIL, kIR, kTop + 54.f);

		// Simulation section
		constexpr float kS1 = kTop + 66.f;
		DrawSectionHeader(ui, "SIMULATION", kAnchor, kL, kIL, kS1);
		DrawSeparator    (ui, kAnchor, kIL, kIR, kS1 + 13.f);

		std::array<char, 64> buf{};

		constexpr float kR1 = kS1 + 34.f;
		std::snprintf(buf.data(), buf.size(), "%d", m_gameSystem->GetActiveBodyCount());
		DrawKV(ui, "Dynamic bodies", buf.data(), kAnchor, kKey, kVal, kR1);

		constexpr float kR2 = kR1 + kRowH;
		std::snprintf(buf.data(), buf.size(), "%d", m_gameSystem->GetProjectileCount());
		DrawKV(ui, "Projectiles", buf.data(), kAnchor, kKey, kVal, kR2);

		constexpr float kR3 = kR2 + kRowH;
		std::snprintf(buf.data(), buf.size(), "%.1f s", m_gameSystem->GetSimTime());
		DrawKV(ui, "Sim time", buf.data(), kAnchor, kKey, kVal, kR3);

		constexpr float kR4 = kR3 + kRowH;
		std::snprintf(buf.data(), buf.size(), "%.0f Hz (fixed)", 1.f / aether::PhysicsSystem::kFixedTimestep);
		DrawKV(ui, "Step rate", buf.data(), kAnchor, kKey, kVal, kR4, kColorGood);

		// Controls section
		constexpr float kCS = kR4 + kRowH + 14.f;
		DrawSectionHeader(ui, "CONTROLS", kAnchor, kL, kIL, kCS);
		DrawSeparator    (ui, kAnchor, kIL, kIR, kCS + 13.f);

		constexpr float kC1 = kCS + 34.f;
		DrawKV(ui, "Space", "Fire projectile", kAnchor, kKey, kVal, kC1, kColorWarn);

		constexpr float kC2 = kC1 + kRowH;
		DrawKV(ui, "R", "Reset scene",     kAnchor, kKey, kVal, kC2);

		constexpr float kC3 = kC2 + kRowH;
		DrawKV(ui, "C", "Toggle camera",   kAnchor, kKey, kVal, kC3);
	}

} // namespace aether::app
