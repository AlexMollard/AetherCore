#pragma once

#include <thread>

#include "scene/Entity.hpp"
#include "scripting/GameplayAbi.h"

namespace aether
{
	class World;
}

namespace aether::scripting
{
	// One access session owns a unique world token. World must outlive this object, or
	// Invalidate must be called before World teardown. No ECS reference crosses the ABI.
	class GameplayAccess
	{
	public:
		using DiagnosticSink = AetherGameplayStatus (*)(void* user, const AetherGameplayDiagnostic& diagnostic);
		enum class ScopeKind
		{
			Callback,
			StructuralMutation,
			PredictedAttachCallback
		};

		class Scope
		{
		public:
			Scope(GameplayAccess& access, ScopeKind kind) noexcept;
			~Scope();
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

			[[nodiscard]] AetherGameplayStatus Status() const noexcept
			{
				return m_status;
			}

		private:
			uint64_t m_context = 0;
			AetherGameplayStatus m_status;
		};
#if defined(__EMSCRIPTEN__)
		// A managed predicted prefab must attach synchronously inside its caller's
		// callback so its one-shot payload remains available to OnAttach.
		class PredictedAttachScope
		{
		public:
			explicit PredictedAttachScope(GameplayAccess& access) noexcept;
			~PredictedAttachScope();
			PredictedAttachScope(const PredictedAttachScope&) = delete;
			PredictedAttachScope& operator=(const PredictedAttachScope&) = delete;
		private:
			GameplayAccess& m_access;
		};
#endif

		explicit GameplayAccess(World& world, DiagnosticSink sink = nullptr, void* sinkUser = nullptr);
		~GameplayAccess();
		GameplayAccess(const GameplayAccess&) = delete;
		GameplayAccess& operator=(const GameplayAccess&) = delete;
		[[nodiscard]] AetherGameplayEngineApi EngineApi() const noexcept;
		[[nodiscard]] AetherGameplayEntity Handle(Entity entity) const noexcept;

		[[nodiscard]] bool MatchesWorld(const World& world) const noexcept
		{
			return m_world == &world;
		}

		void Invalidate() noexcept;

	private:
		static GameplayAccess* Resolve(uint64_t context, AetherGameplayStatus& status) noexcept;
		[[nodiscard]] AetherGameplayStatus Validate(AetherGameplayEntity entity) const noexcept;
		static AetherGameplayStatus EntityValid(uint64_t context, AetherGameplayEntity entity, uint32_t* outValid) noexcept;
		static AetherGameplayStatus GetPosition(uint64_t context, AetherGameplayEntity entity, AetherGameplayVec3* outPosition) noexcept;
		static AetherGameplayStatus SetPosition(uint64_t context, AetherGameplayEntity entity, AetherGameplayVec3 position) noexcept;
		static AetherGameplayStatus ReportDiagnostic(uint64_t context, const AetherGameplayDiagnostic* diagnostic) noexcept;

		World* m_world;
		uint64_t m_context;
		std::thread::id m_owner;
		DiagnosticSink m_sink;
		void* m_sink_user;
		unsigned m_scope_depth = 0;
#if defined(__EMSCRIPTEN__)
		unsigned m_predicted_attach_depth = 0;
#endif
		bool m_reporting = false;
	};
} // namespace aether::scripting
