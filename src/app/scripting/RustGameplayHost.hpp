#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "scripting/AgentDiagnostics.hpp"
#include "scripting/GameplayHost.hpp"

namespace aether
{
	class World;
}

namespace aether::app::scripting
{
	struct SceneContext;

	// Owns the native module. Unload destroys instances before releasing callback code.
	// World and the optional SceneContext must outlive this host's loaded session.
	class RustGameplayHost
	{
	public:
		RustGameplayHost();
		~RustGameplayHost();
		RustGameplayHost(const RustGameplayHost&) = delete;
		RustGameplayHost& operator=(const RustGameplayHost&) = delete;

		GameplayHost& Gameplay()
		{
			return m_gameplay;
		}

		const GameplayHost& Gameplay() const
		{
			return m_gameplay;
		}

		Expected<void> Load(const std::filesystem::path& path, World& world, SceneContext* context = nullptr);
		void Unload();

		[[nodiscard]] const std::vector<aether::scripting::AgentDiagnostic>& Diagnostics() const
		{
			return m_diagnostics;
		}

	private:
		struct Session;
		static AetherGameplayStatus Diagnostic(void* user, const AetherGameplayDiagnostic& diagnostic);
		Expected<void> Failure(std::string code, std::string message);
		GameplayHost m_gameplay;
		std::unique_ptr<Session> m_session;
		std::vector<aether::scripting::AgentDiagnostic> m_diagnostics;
		std::uint32_t m_generation = 0;
	};
} // namespace aether::app::scripting
