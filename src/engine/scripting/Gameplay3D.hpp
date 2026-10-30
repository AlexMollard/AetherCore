#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayServices.h"

namespace aether
{
	class PhysicsSystem;
	struct RigidBodyComponent;
} // namespace aether

namespace aether::scripting::gameplay3d
{
	// Engine-side behaviour behind the 3D gameplay surface. The C# interop exports
	// (src/app/scripting/interop/{Physics,Character,Animation,Ragdoll}Exports.cpp) and the
	// Rust/browser gameplay-service operations 17..30 (ExecuteGameplay3DOperation) both call
	// these, so a body, character, ragdoll or animator behaves identically from every
	// language and backend. Game thread only. Network authority is the caller's concern: the
	// native hosts check it before calling in, the browser has none.
	//
	// Status mapping, identical for every function: a missing component or system is
	// UNSUPPORTED, a malformed argument INVALID_ARGUMENT, a target whose engine object is not
	// created yet NOT_READY. Nothing here fails silently.

	// ---- Rigid bodies ----

	// Seeds RigidBodyComponent::initialVelocity when the Jolt body is not baked yet, so
	// "add a body, then set its velocity" behaves the same either side of the bake.
	AetherGameplayStatus SetLinearVelocity(World& world, PhysicsSystem* physics, Entity entity, const glm::vec3& velocity);
	AetherGameplayStatus GetLinearVelocity(World& world, PhysicsSystem* physics, Entity entity, glm::vec3& out);
	// Forces and impulses have no persisted initial state to seed, so before the body is baked
	// they return NOT_READY (and warn once per body) instead of being dropped silently.
	AetherGameplayStatus AddForce(World& world, PhysicsSystem* physics, Entity entity, const glm::vec3& force);
	AetherGameplayStatus AddImpulse(World& world, PhysicsSystem* physics, Entity entity, const glm::vec3& impulse);
	// Warns exactly once per RigidBodyComponent that `what` had nothing to act on.
	void WarnDeferredBodyCall(RigidBodyComponent& body, std::uint32_t entityId, const char* what);

	// ---- Queries ----

	struct RayHit
	{
		bool hit = false;
		std::uint32_t entity = 0;
		float distance = 0.0f;
		glm::vec3 point{0.0f};
		glm::vec3 normal{0.0f};
	};

	// `direction` need not be normalized but must be finite and non-zero; `maxDistance` must be
	// finite and positive. A miss is OK with hit == false.
	AetherGameplayStatus Raycast(PhysicsSystem* physics, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, RayHit& out);

	// ---- Character controller ----

	AetherGameplayStatus CharacterMove(World& world, Entity entity, const glm::vec3& direction, float speed);
	AetherGameplayStatus CharacterJump(World& world, Entity entity, float speed);
	AetherGameplayStatus CharacterIsGrounded(World& world, Entity entity, bool& out);

	// ---- Ragdoll ----

	// Loads the skeleton at `skeletonPath` (a VFS path) synchronously and builds a ragdoll on
	// `source`, which becomes its root. `bones` (root included) is filled on success.
	AetherGameplayStatus SpawnRagdoll(World& world, Entity source, std::string_view skeletonPath, std::vector<Entity>& bones);

	// ---- Skinned animation ----

	// Where an animator's clip names come from when its SkinnedMeshComponent has no
	// AnimationDatabase. The native engine always has one (and ignores the catalog). The browser
	// never does: its clips are cooked glTF owned by the model loader, which implements this.
	// `animator` is the entity that carries the SkinnedMeshComponent. Clips owned by the
	// component itself (pendingExternalAnims) are counted after the catalog's.
	class ClipCatalog
	{
	public:
		virtual ~ClipCatalog() = default;
		[[nodiscard]] virtual std::uint32_t ClipCount(Entity animator) const = 0;
		[[nodiscard]] virtual std::string_view ClipName(Entity animator, std::uint32_t index) const = 0;
		// Longest channel key time of the clip, like AnimationDatabase::GetClipDuration.
		[[nodiscard]] virtual float ClipDuration(Entity animator, std::uint32_t index) const = 0;
		[[nodiscard]] virtual std::optional<std::uint32_t> FindNode(Entity, std::string_view) const = 0;
		virtual void Remap(Entity animator, assets::GltfAnimation& animation) const = 0;
		virtual void Compile(World& world, Entity entity) const = 0;
	};

	struct Animator
	{
		Entity entity{};
		SkinnedMeshComponent* smc = nullptr;
	};

	// The entity's own SkinnedMeshComponent, else its first child's (a spawned model).
	[[nodiscard]] Animator FindAnimator(World& world, std::uint32_t id);
	[[nodiscard]] SkinnedMeshComponent* FindSmcOrSpawned(World& world, std::uint32_t id);
	// Calls `apply` on every SkinnedMeshComponent among the entity's direct children.
	template<typename Fn>
	void ForEachSpawnedSmc(World& world, std::uint32_t id, Fn&& apply);
	// Applies `apply` to the entity's own SkinnedMeshComponent and its spawned children.
	// Returns how many animators it reached.
	template<typename Fn>
	std::size_t ForEachAnimator(World& world, std::uint32_t id, Fn&& apply);

	[[nodiscard]] std::uint32_t ClipCount(const Animator& animator, const ClipCatalog* clips = nullptr);
	// Name of clip `index` (database/catalog clips first, then pending external ones); empty when out of range.
	[[nodiscard]] std::string_view ClipName(const Animator& animator, std::uint32_t index, const ClipCatalog* clips = nullptr);
	// Length in seconds of clip `index` (longest channel key time); 0 when out of range.
	[[nodiscard]] float ClipDuration(const Animator& animator, std::uint32_t index, const ClipCatalog* clips = nullptr);
	// Index of the clip named `name` (database/catalog clips first, then pending external ones), or -1.
	[[nodiscard]] std::int32_t FindClip(const Animator& animator, std::string_view name, const ClipCatalog* clips = nullptr);
	// Starts clip `clip` on every animator of the entity, blending from the playing clip over
	// `seconds` (0 switches outright). The index is not range-checked; see ResolveClip.
	void CrossFade(World& world, std::uint32_t id, std::uint32_t clip, float seconds);
	// Resolves a clip by name (non-empty) or index; INVALID_ARGUMENT when it does not exist.
	AetherGameplayStatus ResolveClip(World& world, Entity entity, std::string_view name, std::int64_t index, std::uint32_t& out, const ClipCatalog* clips = nullptr);
	AetherGameplayStatus PlayClip(World& world, Entity entity, std::string_view name, std::int64_t index, float fadeSeconds, const ClipCatalog* clips = nullptr);
	AetherGameplayStatus SetPlaybackSpeed(World& world, Entity entity, float speed);
	AetherGameplayStatus GetAnimTime(World& world, Entity entity, float& out);
	AetherGameplayStatus GetCurrentClip(World& world, Entity entity, std::uint32_t& out);

	// ---- Gameplay service operations 17..30 ----

	[[nodiscard]] constexpr bool IsGameplay3DOperation(std::uint32_t operation) noexcept
	{
		return operation >= AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D && operation <= AE_GAMEPLAY_SERVICE_GET_CURRENT_CLIP;
	}

	// Operations whose entity argument must be a live entity (the host validates it first).
	[[nodiscard]] constexpr bool IsGameplay3DEntityOperation(std::uint32_t operation) noexcept
	{
		return IsGameplay3DOperation(operation) && operation != AE_GAMEPLAY_SERVICE_RAYCAST_3D;
	}

	// Operations whose text2 is a raw binary payload, not UTF-8.
	[[nodiscard]] constexpr bool HasRawText2(std::uint32_t operation) noexcept
	{
		return operation == AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS || operation == AE_GAMEPLAY_SERVICE_RAYCAST_3D;
	}

	// Operations that write a raw binary result into the string buffer.
	[[nodiscard]] constexpr bool HasRawResult(std::uint32_t operation) noexcept
	{
		return operation == AE_GAMEPLAY_SERVICE_RAYCAST_3D;
	}

	struct OperationResult
	{
		AetherGameplayStatus status = AE_GAMEPLAY_OK;
		// Set by operations that return an entity; the host turns it into a handle.
		bool hasEntity = false;
		Entity entity{};
	};

	// Runs one operation against the world. For IsGameplay3DEntityOperation operations the host
	// has already validated `request.entity`. Entity results (raycast hit, ragdoll root) come
	// back in OperationResult for the host to turn into a handle.
	// Writes `value` (and the raw result into `buffer` for HasRawResult operations).
	OperationResult ExecuteGameplay3DOperation(World& world, PhysicsSystem* physics, const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& value, AetherGameplayUtf8Buffer buffer, const ClipCatalog* clips = nullptr);

	// Copies raw bytes plus a trailing NUL into `destination` with the same BUFFER_TOO_SMALL
	// contract as CopyGameplayUtf8, without the UTF-8 check.
	AetherGameplayStatus CopyGameplayBytes(const void* data, std::uint32_t length, AetherGameplayUtf8Buffer destination) noexcept;
} // namespace aether::scripting::gameplay3d

namespace aether::scripting::gameplay3d
{
	template<typename Fn>
	void ForEachSpawnedSmc(World& world, std::uint32_t id, Fn&& apply)
	{
		const auto* hierarchy = world.TryGet<HierarchyComponent>(Entity{id});
		if (hierarchy == nullptr)
		{
			return;
		}
		for (const Entity child: hierarchy->children)
		{
			if (auto* smc = world.TryGet<SkinnedMeshComponent>(child))
			{
				apply(*smc);
			}
		}
	}

	template<typename Fn>
	std::size_t ForEachAnimator(World& world, std::uint32_t id, Fn&& apply)
	{
		std::size_t reached = 0;
		if (auto* smc = world.TryGet<SkinnedMeshComponent>(Entity{id}))
		{
			apply(*smc);
			++reached;
		}
		ForEachSpawnedSmc(world, id, [&](SkinnedMeshComponent& child) { apply(child); ++reached; });
		return reached;
	}
} // namespace aether::scripting::gameplay3d
