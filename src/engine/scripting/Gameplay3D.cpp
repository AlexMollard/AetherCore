#include "scripting/Gameplay3D.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#if !defined(AETHERCORE_WEB_RUNTIME)
#	include "animation/AnimationDatabase.hpp"
#endif
#include "animation/AnimationSystem.hpp"
#include "assets/GltfAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics/RagdollBuilder.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::scripting::gameplay3d
{
	namespace
	{
		[[nodiscard]] bool Alive(World& world, Entity entity)
		{
			return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
		}

		[[nodiscard]] bool Finite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}
	} // namespace

	void WarnDeferredBodyCall(RigidBodyComponent& body, std::uint32_t entityId, const char* what)
	{
		if (body.deferredCallWarned)
		{
			return;
		}
		body.deferredCallWarned = true;
		AE_WARN(LogCategory::Engine,
		        "Physics.{} on entity {} had no effect: its body has not been created yet "
		        "(queued this frame, baked next). If this keeps recurring for the same "
		        "entity, the body failed to create - look for FlushPendingBodies' own "
		        "warning naming it.",
		        what, entityId);
	}

	AetherGameplayStatus SetLinearVelocity(World& world, PhysicsSystem* physics, Entity entity, const glm::vec3& velocity)
	{
		auto* body = world.TryGet<RigidBodyComponent>(entity);
		if (physics == nullptr || body == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		if (body->body.IsValid())
		{
			physics->SetLinearVelocity(body->body, velocity);
		}
		else
		{
			// Body not baked yet (queued this frame, created next by FlushPendingBodies) - seed the
			// value creation itself applies.
			body->initialVelocity = velocity;
		}
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus GetLinearVelocity(World& world, PhysicsSystem* physics, Entity entity, glm::vec3& out)
	{
		const auto* body = world.TryGet<RigidBodyComponent>(entity);
		if (body == nullptr || (physics == nullptr && body->body.IsValid()))
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		// Mirrors the setter: before the body exists its velocity IS whatever was seeded.
		out = body->body.IsValid() ? physics->GetLinearVelocity(body->body) : body->initialVelocity;
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus AddForce(World& world, PhysicsSystem* physics, Entity entity, const glm::vec3& force)
	{
		auto* body = world.TryGet<RigidBodyComponent>(entity);
		if (physics == nullptr || body == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		if (!body->body.IsValid())
		{
			// A continuous per-step force has no persisted initial state to seed.
			WarnDeferredBodyCall(*body, entity.id, "AddForce");
			return AE_GAMEPLAY_NOT_READY;
		}
		physics->AddForce(body->body, force);
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus AddImpulse(World& world, PhysicsSystem* physics, Entity entity, const glm::vec3& impulse)
	{
		auto* body = world.TryGet<RigidBodyComponent>(entity);
		if (physics == nullptr || body == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		if (!body->body.IsValid())
		{
			// An impulse divides by a mass that is not resolved until the body is created.
			WarnDeferredBodyCall(*body, entity.id, "AddImpulse");
			return AE_GAMEPLAY_NOT_READY;
		}
		physics->AddImpulse(body->body, impulse);
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus Raycast(PhysicsSystem* physics, const glm::vec3& origin, const glm::vec3& direction, float maxDistance, RayHit& out)
	{
		out = {};
		if (!Finite(origin) || !Finite(direction) || !std::isfinite(maxDistance) || maxDistance <= 0.0f || glm::length(direction) < 0.001f)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		if (physics == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		const auto result = physics->CastRay(origin, direction, maxDistance);
		if (result.hit)
		{
			out.hit = true;
			out.entity = result.entity;
			out.distance = result.fraction * maxDistance;
			out.point = result.position;
			out.normal = result.normal;
		}
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus CharacterMove(World& world, Entity entity, const glm::vec3& direction, float speed)
	{
		auto* controller = world.TryGet<CharacterControllerComponent>(entity);
		if (controller == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		// Any vertical component is stripped by StepCharacters, which owns vertical velocity.
		controller->desiredVelocity = direction * speed;
		controller->velocityOverride = false;
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus CharacterJump(World& world, Entity entity, float speed)
	{
		auto* controller = world.TryGet<CharacterControllerComponent>(entity);
		if (controller == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		controller->pendingJumpSpeed = speed;
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus CharacterIsGrounded(World& world, Entity entity, bool& out)
	{
		const auto* controller = world.TryGet<CharacterControllerComponent>(entity);
		if (controller == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		out = controller->isGrounded;
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus SpawnRagdoll(World& world, Entity source, std::string_view skeletonPath, std::vector<Entity>& bones)
	{
		bones.clear();
		if (!Alive(world, source))
		{
			return AE_GAMEPLAY_INVALID_HANDLE;
		}
		// Read purely for its bind-pose skeleton, synchronously: a ragdoll spawn is a rare one-shot
		// event that needs none of the asynchronous model pipeline (GPU upload, draw-ready meshes).
		auto skeleton = assets::GltfAsset::LoadFromVfsPath(skeletonPath);
		if (!skeleton.has_value())
		{
			AE_WARN(LogCategory::App, "Ragdoll.Spawn: could not load skeleton '{}': {}", skeletonPath, skeleton.error());
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		if (!aether::SpawnRagdoll(world, source, *skeleton))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		// Read straight off the RagdollComponent SpawnRagdoll populated, so it cannot disagree with
		// what was actually created.
		if (const auto* ragdoll = world.TryGet<RagdollComponent>(source))
		{
			bones = ragdoll->bones;
		}
		return AE_GAMEPLAY_OK;
	}

	Animator FindAnimator(World& world, std::uint32_t id)
	{
		const Entity entity{id};
		if (auto* smc = world.TryGet<SkinnedMeshComponent>(entity))
		{
			return {entity, smc};
		}
		const auto* hierarchy = world.TryGet<HierarchyComponent>(entity);
		if (hierarchy != nullptr && !hierarchy->children.empty())
		{
			const Entity child = hierarchy->children.front();
			return {child, world.TryGet<SkinnedMeshComponent>(child)};
		}
		return {};
	}

	SkinnedMeshComponent* FindSmcOrSpawned(World& world, std::uint32_t id)
	{
		return FindAnimator(world, id).smc;
	}

	namespace
	{
		// Clips that are not the component's own pending external ones: the AnimationDatabase's on
		// the native engine, the host catalog's where no database exists (the browser).
		std::uint32_t BaseClipCount(const Animator& animator, const ClipCatalog* clips)
		{
#if !defined(AETHERCORE_WEB_RUNTIME)
			if (animator.smc->animDb != nullptr)
			{
				return animator.smc->animDb->GetClipCount();
			}
#endif
			return clips != nullptr ? clips->ClipCount(animator.entity) : 0;
		}

		std::string_view BaseClipName(const Animator& animator, const ClipCatalog* clips, std::uint32_t index)
		{
#if !defined(AETHERCORE_WEB_RUNTIME)
			if (animator.smc->animDb != nullptr)
			{
				return animator.smc->animDb->GetClipName(index);
			}
#endif
			return clips != nullptr ? clips->ClipName(animator.entity, index) : std::string_view{};
		}

		float BaseClipDuration(const Animator& animator, const ClipCatalog* clips, std::uint32_t index)
		{
#if !defined(AETHERCORE_WEB_RUNTIME)
			if (animator.smc->animDb != nullptr)
			{
				return animator.smc->animDb->GetClipDuration(index);
			}
#endif
			return clips != nullptr ? clips->ClipDuration(animator.entity, index) : 0.0f;
		}
	} // namespace

	std::uint32_t ClipCount(const Animator& animator, const ClipCatalog* clips)
	{
		return BaseClipCount(animator, clips) + static_cast<std::uint32_t>(animator.smc->pendingExternalAnims.size());
	}

	std::string_view ClipName(const Animator& animator, std::uint32_t index, const ClipCatalog* clips)
	{
		const std::uint32_t base = BaseClipCount(animator, clips);
		if (index < base)
		{
			return BaseClipName(animator, clips, index);
		}
		const auto& pending = animator.smc->pendingExternalAnims;
		return index - base < pending.size() ? std::string_view(pending[index - base].name) : std::string_view{};
	}

	float ClipDuration(const Animator& animator, std::uint32_t index, const ClipCatalog* clips)
	{
		const std::uint32_t base = BaseClipCount(animator, clips);
		if (index < base)
		{
			return BaseClipDuration(animator, clips, index);
		}
		const auto& pending = animator.smc->pendingExternalAnims;
		if (index - base >= pending.size())
		{
			return 0.0f;
		}
		float duration = 0.0f;
		for (const auto& channel: pending[index - base].channels)
		{
			if (!channel.times.empty())
			{
				duration = std::max(duration, channel.times.back());
			}
		}
		return duration;
	}

	std::int32_t FindClip(const Animator& animator, std::string_view name, const ClipCatalog* clips)
	{
		const std::uint32_t base = BaseClipCount(animator, clips);
		for (std::uint32_t i = 0; i < base; ++i)
		{
			if (BaseClipName(animator, clips, i) == name)
			{
				return static_cast<std::int32_t>(i);
			}
		}
		const auto& pending = animator.smc->pendingExternalAnims;
		for (std::uint32_t i = 0; i < pending.size(); ++i)
		{
			if (pending[i].name == name)
			{
				return static_cast<std::int32_t>(base + i);
			}
		}
		return -1;
	}

	void CrossFade(World& world, std::uint32_t id, std::uint32_t clip, float seconds)
	{
		ForEachAnimator(world, id, [clip, seconds](SkinnedMeshComponent& smc) { CrossFadeTo(smc, clip, seconds); });
	}

	AetherGameplayStatus ResolveClip(World& world, Entity entity, std::string_view name, std::int64_t index, std::uint32_t& out, const ClipCatalog* clips)
	{
		const auto animator = FindAnimator(world, entity.id);
		if (animator.smc == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		if (!name.empty())
		{
			const auto found = FindClip(animator, name, clips);
			if (found < 0)
			{
				return AE_GAMEPLAY_INVALID_ARGUMENT;
			}
			out = static_cast<std::uint32_t>(found);
			return AE_GAMEPLAY_OK;
		}
		if (index < 0 || index >= static_cast<std::int64_t>(ClipCount(animator, clips)))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		out = static_cast<std::uint32_t>(index);
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus PlayClip(World& world, Entity entity, std::string_view name, std::int64_t index, float fadeSeconds, const ClipCatalog* clips)
	{
		if (!std::isfinite(fadeSeconds) || fadeSeconds < 0.0f)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		std::uint32_t clip = 0;
		if (const auto status = ResolveClip(world, entity, name, index, clip, clips); status != AE_GAMEPLAY_OK)
		{
			return status;
		}
		CrossFade(world, entity.id, clip, fadeSeconds);
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus SetPlaybackSpeed(World& world, Entity entity, float speed)
	{
		if (!std::isfinite(speed))
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		return ForEachAnimator(world, entity.id, [speed](SkinnedMeshComponent& smc) { smc.playbackSpeed = speed; }) != 0 ? AE_GAMEPLAY_OK : AE_GAMEPLAY_UNSUPPORTED;
	}

	AetherGameplayStatus GetAnimTime(World& world, Entity entity, float& out)
	{
		const auto* smc = FindSmcOrSpawned(world, entity.id);
		if (smc == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		out = smc->animTime;
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus GetCurrentClip(World& world, Entity entity, std::uint32_t& out)
	{
		const auto* smc = FindSmcOrSpawned(world, entity.id);
		if (smc == nullptr)
		{
			return AE_GAMEPLAY_UNSUPPORTED;
		}
		out = smc->clipIndex;
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus CopyGameplayBytes(const void* data, std::uint32_t length, AetherGameplayUtf8Buffer destination) noexcept
	{
		if (destination.required == nullptr || length == std::numeric_limits<std::uint32_t>::max() || (destination.data == nullptr && destination.capacity != 0) || (length != 0 && data == nullptr))
		{
			if (destination.data != nullptr && destination.capacity != 0)
			{
				destination.data[0] = '\0';
			}
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		*destination.required = length + 1;
		if (destination.capacity < *destination.required)
		{
			if (destination.data != nullptr && destination.capacity != 0)
			{
				destination.data[0] = '\0';
			}
			return AE_GAMEPLAY_BUFFER_TOO_SMALL;
		}
		if (length != 0)
		{
			std::memmove(destination.data, data, length);
		}
		destination.data[length] = '\0';
		return AE_GAMEPLAY_OK;
	}

	OperationResult ExecuteGameplay3DOperation(World& world, PhysicsSystem* physics, const AetherGameplayServiceRequest& request, AetherGameplayPropertyValue& value, AetherGameplayUtf8Buffer buffer, const ClipCatalog* clips)
	{
		const Entity entity{request.entity.id};
		const glm::vec3 vector{request.vector[0], request.vector[1], request.vector[2]};
		const std::string_view text(request.text.data != nullptr ? request.text.data : "", request.text.length);
		OperationResult result;
		const auto fail = [&result](AetherGameplayStatus status)
		{
			result.status = status;
			return result;
		};
		switch (request.operation)
		{
			case AE_GAMEPLAY_SERVICE_GET_VELOCITY_3D:
			{
				glm::vec3 velocity{};
				if (const auto status = GetLinearVelocity(world, physics, entity, velocity); status != AE_GAMEPLAY_OK)
				{
					return fail(status);
				}
				value.type = AE_GAMEPLAY_PROPERTY_VECTOR3;
				value.vector[0] = velocity.x;
				value.vector[1] = velocity.y;
				value.vector[2] = velocity.z;
				return result;
			}
			case AE_GAMEPLAY_SERVICE_SET_VELOCITY_3D:
				return fail(SetLinearVelocity(world, physics, entity, vector));
			case AE_GAMEPLAY_SERVICE_ADD_FORCE_3D:
				return fail(AddForce(world, physics, entity, vector));
			case AE_GAMEPLAY_SERVICE_ADD_IMPULSE_3D:
				return fail(AddImpulse(world, physics, entity, vector));
			case AE_GAMEPLAY_SERVICE_RAYCAST_3D:
			{
				float payload[7];
				static_assert(sizeof(payload) == AE_GAMEPLAY_RAYCAST_PAYLOAD_BYTES);
				if (request.text2.data == nullptr || request.text2.length != sizeof(payload))
				{
					return fail(AE_GAMEPLAY_INVALID_ARGUMENT);
				}
				std::memcpy(payload, request.text2.data, sizeof(payload));
				RayHit hit;
				if (const auto status = Raycast(physics, {payload[0], payload[1], payload[2]}, {payload[3], payload[4], payload[5]}, payload[6], hit); status != AE_GAMEPLAY_OK)
				{
					return fail(status);
				}
				const float normal[3] = {hit.normal.x, hit.normal.y, hit.normal.z};
				if (const auto status = CopyGameplayBytes(normal, sizeof(normal), buffer); status != AE_GAMEPLAY_OK)
				{
					return fail(status);
				}
				value.type = AE_GAMEPLAY_PROPERTY_VECTOR3;
				value.integer = hit.hit ? 1 : 0;
				value.vector[0] = hit.point.x;
				value.vector[1] = hit.point.y;
				value.vector[2] = hit.point.z;
				value.vector[3] = hit.distance;
				if (hit.hit && hit.entity != 0)
				{
					result.hasEntity = true;
					result.entity = Entity{hit.entity};
				}
				return result;
			}
			case AE_GAMEPLAY_SERVICE_CHARACTER_MOVE:
				return fail(CharacterMove(world, entity, vector, request.vector[3]));
			case AE_GAMEPLAY_SERVICE_CHARACTER_JUMP:
				if (request.vector[0] < 0.0f)
				{
					return fail(AE_GAMEPLAY_INVALID_ARGUMENT);
				}
				return fail(CharacterJump(world, entity, request.vector[0]));
			case AE_GAMEPLAY_SERVICE_CHARACTER_IS_GROUNDED:
			{
				bool grounded = false;
				if (const auto status = CharacterIsGrounded(world, entity, grounded); status != AE_GAMEPLAY_OK)
				{
					return fail(status);
				}
				value.type = AE_GAMEPLAY_PROPERTY_BOOL;
				value.integer = grounded ? 1 : 0;
				return result;
			}
			case AE_GAMEPLAY_SERVICE_SPAWN_RAGDOLL:
			{
				std::vector<Entity> bones;
				if (const auto status = SpawnRagdoll(world, entity, text, bones); status != AE_GAMEPLAY_OK)
				{
					return fail(status);
				}
				value.type = AE_GAMEPLAY_PROPERTY_ENTITY;
				value.integer = static_cast<std::int64_t>(bones.size());
				result.hasEntity = true;
				result.entity = entity;
				return result;
			}
			case AE_GAMEPLAY_SERVICE_SET_ANIMATION_CLIP:
				return fail(PlayClip(world, entity, text, request.integer, request.vector[0], clips));
			case AE_GAMEPLAY_SERVICE_SET_ANIMATION_SPEED:
				return fail(SetPlaybackSpeed(world, entity, request.vector[0]));
			case AE_GAMEPLAY_SERVICE_GET_ANIMATION_TIME:
			{
				float time = 0.0f;
				if (const auto status = GetAnimTime(world, entity, time); status != AE_GAMEPLAY_OK)
				{
					return fail(status);
				}
				value.type = AE_GAMEPLAY_PROPERTY_FLOAT;
				value.vector[0] = time;
				return result;
			}
			case AE_GAMEPLAY_SERVICE_FIND_ANIMATION_CLIP:
			{
				const auto animator = FindAnimator(world, entity.id);
				if (animator.smc == nullptr)
				{
					return fail(AE_GAMEPLAY_UNSUPPORTED);
				}
				const auto clip = text.empty() ? -1 : FindClip(animator, text, clips);
				if (clip < 0)
				{
					return fail(AE_GAMEPLAY_INVALID_ARGUMENT);
				}
				value.type = AE_GAMEPLAY_PROPERTY_INT;
				value.integer = clip;
				return result;
			}
			case AE_GAMEPLAY_SERVICE_GET_CURRENT_CLIP:
			{
				std::uint32_t clip = 0;
				if (const auto status = GetCurrentClip(world, entity, clip); status != AE_GAMEPLAY_OK)
				{
					return fail(status);
				}
				value.type = AE_GAMEPLAY_PROPERTY_INT;
				value.integer = clip;
				return result;
			}
			default:
				return fail(AE_GAMEPLAY_UNSUPPORTED);
		}
	}
} // namespace aether::scripting::gameplay3d
