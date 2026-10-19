#include "GameplayAccess.hpp"

#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "scene/TransformEdit.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayAbiValidation.hpp"

namespace aether::scripting
{
	namespace
	{
		std::mutex s_access_mutex;
		std::unordered_map<uint64_t, GameplayAccess*> s_access;
		std::atomic<uint64_t> s_next_context{1};
	} // namespace

	GameplayAccess::GameplayAccess(World& world, DiagnosticSink sink, void* sinkUser)
	      : m_world(&world), m_context(s_next_context.fetch_add(1)), m_owner(std::this_thread::get_id()), m_sink(sink), m_sink_user(sinkUser)
	{
		std::lock_guard lock(s_access_mutex);
		s_access.emplace(m_context, this);
	}

	GameplayAccess::~GameplayAccess()
	{
		Invalidate();
	}

	void GameplayAccess::Invalidate() noexcept
	{
		std::lock_guard lock(s_access_mutex);
		s_access.erase(m_context);
		m_world = nullptr;
	}

	GameplayAccess* GameplayAccess::Resolve(uint64_t context, AetherGameplayStatus& status) noexcept
	{
		const auto entry = s_access.find(context);
		if (entry == s_access.end())
		{
			status = AE_GAMEPLAY_INVALID_HANDLE;
			return nullptr;
		}
		auto* access = entry->second;
		if (access->m_owner != std::this_thread::get_id())
		{
			status = AE_GAMEPLAY_WRONG_THREAD;
			return nullptr;
		}
		status = AE_GAMEPLAY_OK;
		return access;
	}

	AetherGameplayEngineApi GameplayAccess::EngineApi() const noexcept
	{
		return {sizeof(AetherGameplayEngineApi), AE_GAMEPLAY_ABI_MAJOR, 0, m_context, EntityValid, GetPosition, SetPosition, ReportDiagnostic};
	}

	AetherGameplayEntity GameplayAccess::Handle(Entity entity) const noexcept
	{
		return {m_context, entity.id, 0};
	}

	AetherGameplayStatus GameplayAccess::Validate(AetherGameplayEntity entity) const noexcept
	{
		if (entity.world != m_context)
		{
			return AE_GAMEPLAY_WRONG_WORLD;
		}
		if (entity.reserved != 0)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		return m_world->GetRegistry().valid(World::ToEntt(Entity{entity.id})) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_INVALID_HANDLE;
	}

	AetherGameplayStatus GameplayAccess::EntityValid(uint64_t context, AetherGameplayEntity entity, uint32_t* outValid) noexcept
	{
		if (outValid == nullptr)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		*outValid = 0;
		std::lock_guard lock(s_access_mutex);
		AetherGameplayStatus status;
		auto* access = Resolve(context, status);
		if (access == nullptr)
		{
			return status;
		}
		status = access->Validate(entity);
		*outValid = status == AE_GAMEPLAY_OK ? 1u : 0u;
		return status;
	}

	AetherGameplayStatus GameplayAccess::GetPosition(uint64_t context, AetherGameplayEntity entity, AetherGameplayVec3* outPosition) noexcept
	{
		if (outPosition == nullptr)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		*outPosition = {};
		std::lock_guard lock(s_access_mutex);
		AetherGameplayStatus status;
		auto* access = Resolve(context, status);
		if (access == nullptr)
		{
			return status;
		}
		status = access->Validate(entity);
		if (status != AE_GAMEPLAY_OK)
		{
			return status;
		}
		const auto* transform = access->m_world->TryGet<TransformComponent>(Entity{entity.id});
		if (transform == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		const auto& translation = transform->localToWorld[3];
		*outPosition = {translation.x, translation.y, translation.z};
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus GameplayAccess::SetPosition(uint64_t context, AetherGameplayEntity entity, AetherGameplayVec3 position) noexcept
	{
		if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		std::lock_guard lock(s_access_mutex);
		AetherGameplayStatus status;
		auto* access = Resolve(context, status);
		if (access == nullptr)
		{
			return status;
		}
		status = access->Validate(entity);
		if (status != AE_GAMEPLAY_OK)
		{
			return status;
		}
		const auto* transform = access->m_world->TryGet<TransformComponent>(Entity{entity.id});
		if (transform == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		auto matrix = transform->localToWorld;
		matrix[3].x = position.x;
		matrix[3].y = position.y;
		matrix[3].z = position.z;
		ecs::SetWorldTransform(*access->m_world, Entity{entity.id}, matrix);
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus GameplayAccess::ReportDiagnostic(uint64_t context, const AetherGameplayDiagnostic* diagnostic) noexcept
	{
		if (diagnostic == nullptr || diagnostic->structSize != sizeof(*diagnostic) || diagnostic->severity > AE_GAMEPLAY_DIAGNOSTIC_ERROR || diagnostic->code.length == 0 || diagnostic->message.length == 0 || !IsGameplayUtf8(diagnostic->code)
		        || !IsGameplayUtf8(diagnostic->message) || !IsGameplayUtf8(diagnostic->file))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		DiagnosticSink sink;
		void* sinkUser;
		{
			std::lock_guard lock(s_access_mutex);
			AetherGameplayStatus status;
			auto* access = Resolve(context, status);
			if (access == nullptr)
			{
				return status;
			}
			if (access->m_reporting)
			{
				return AE_GAMEPLAY_REENTRANT;
			}
			access->m_reporting = true;
			sink = access->m_sink;
			sinkUser = access->m_sink_user;
		}
		AetherGameplayStatus result = AE_GAMEPLAY_UNSUPPORTED;
		try
		{
			if (sink != nullptr)
			{
				result = sink(sinkUser, *diagnostic);
			}
		}
		catch (...)
		{
			result = AE_GAMEPLAY_SCRIPT_ERROR;
		}
		std::lock_guard lock(s_access_mutex);
		const auto entry = s_access.find(context);
		if (entry != s_access.end())
		{
			entry->second->m_reporting = false;
		}
		return result;
	}

	GameplayAccess::Scope::Scope(GameplayAccess& access, ScopeKind kind) noexcept
	{
		(void) kind; // Both kinds are exclusive; engine getters/setters remain available inside callbacks.
		std::lock_guard lock(s_access_mutex);
		auto* resolved = Resolve(access.m_context, m_status);
		if (resolved == nullptr)
		{
			return;
		}
		if (resolved->m_scope_active)
		{
			m_status = AE_GAMEPLAY_REENTRANT;
			return;
		}
		resolved->m_scope_active = true;
		m_context = access.m_context;
	}

	GameplayAccess::Scope::~Scope()
	{
		if (m_context != 0)
		{
			std::lock_guard lock(s_access_mutex);
			const auto entry = s_access.find(m_context);
			if (entry != s_access.end())
			{
				entry->second->m_scope_active = false;
			}
		}
	}
} // namespace aether::scripting
