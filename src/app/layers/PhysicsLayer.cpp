#include "PhysicsLayer.hpp"

#include <imgui.h>

#include "Logger.hpp"
#include "systems/PhysicsGameSystem.hpp"
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
		gameSystem->Init(context.engine, *context.assets, *context.cameras, *context.input, *m_physics);
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
		m_physics = nullptr;
	}

	void PhysicsLayer::OnUpdate([[maybe_unused]] LayerContext& context)
	{
	}

	void PhysicsLayer::OnGui([[maybe_unused]] LayerContext& context)
	{
		if (!m_gameSystem)
		{
			return;
		}

		static constexpr ImVec4 kGood{ 0.40f, 0.72f, 0.46f, 1.f };
		static constexpr ImVec4 kWarn{ 0.86f, 0.71f, 0.30f, 1.f };

		ImGui::SetNextWindowPos(ImVec2(12.f, 12.f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("PHYSICS", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			return;
		}

		ImGui::SeparatorText("SIMULATION");
		ImGui::Columns(2, "##sim", false);

		ImGui::Text("Dynamic bodies");
		ImGui::NextColumn();
		ImGui::Text("%d", m_gameSystem->GetActiveBodyCount());
		ImGui::NextColumn();

		ImGui::Text("Projectiles");
		ImGui::NextColumn();
		ImGui::Text("%d", m_gameSystem->GetProjectileCount());
		ImGui::NextColumn();

		ImGui::Text("Sim time");
		ImGui::NextColumn();
		ImGui::Text("%.1f s", m_gameSystem->GetSimTime());
		ImGui::NextColumn();

		ImGui::Text("Step rate");
		ImGui::NextColumn();
		ImGui::TextColored(kGood, "%.0f Hz (fixed)", 1.f / aether::PhysicsSystem::kFixedTimestep);
		ImGui::NextColumn();

		ImGui::Columns(1);
		ImGui::SeparatorText("CONTROLS");
		ImGui::Columns(2, "##ctrl", false);

		ImGui::Text("Space");
		ImGui::NextColumn();
		ImGui::TextColored(kWarn, "Fire projectile");
		ImGui::NextColumn();
		ImGui::Text("R");
		ImGui::NextColumn();
		ImGui::TextUnformatted("Reset scene");
		ImGui::NextColumn();
		ImGui::Text("C");
		ImGui::NextColumn();
		ImGui::TextUnformatted("Toggle camera");
		ImGui::NextColumn();

		ImGui::Columns(1);
		ImGui::End();
	}

} // namespace aether::app
