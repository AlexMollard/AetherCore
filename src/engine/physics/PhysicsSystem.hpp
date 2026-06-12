#pragma once

#include <memory>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <entt/entt.hpp>

#include "scene/System.hpp"
#include "scene/Entity.hpp"
#include "physics/PhysicsComponents.hpp"

// Forward-declare heavy Jolt internals so this header stays light.
namespace JPH
{
	class PhysicsSystem;
	class TempAllocatorImpl;
	class JobSystemThreadPool;
} // namespace JPH

namespace aether
{
	class World;

	// -- PhysicsSystem ---------------------------------------------------------
	//
	// Owns the Jolt physics world and drives it with a fixed timestep.
	// Using a fixed step (kFixedTimestep = 1/60 s) is essential for:
	//   - determinism across machines (required for lockstep networking)
	//   - stable simulation regardless of render framerate
	//
	// Usage:
	//   world.RegisterSystem(std::make_unique<PhysicsSystem>());
	//
	//   // Spawn a physics-backed entity by emplacing a descriptor + transform:
	//   auto e = world.Create();
	//   world.Emplace<TransformComponent>(e, ...);       // position only, no scale
	//   world.Emplace<BoxBodyDesc>(e, BoxBodyDesc{       // engine handles the rest
	//       .halfExtents = {1, 1, 1},
	//       .motionType  = PhysicsMotionType::Static,
	//   });
	//
	//   // PhysicsSystem processes the descriptor on the next Update, creates the
	//   // Jolt body, sets the correct scaled transform, and removes the descriptor.
	//
	class PhysicsSystem final : public System
	{
	public:
		PhysicsSystem();
		~PhysicsSystem() override;

		// System interface
		void OnRegister(World& world) override;
		void Update(World& world, float dt) override;
		void OnUnregister(World& world) override;

		const char* GetName() const override
		{
			return "PhysicsSystem";
		}

		// Remove the physics body associated with an entity and strip the
		// RigidBodyComponent / PhysicsStateComponent from it.
		void RemoveBody(World& world, Entity entity);

		// Called via entt sink when a RigidBodyComponent is destroyed.
		// Removes and destroys the backing Jolt body so we never leak physics
		// bodies when entities are destroyed outside of RemoveBody().
		void OnRigidBodyDestroyed(entt::registry& registry, entt::entity enttEntity);

		// -- Body control ------------------------------------------------------

		void SetLinearVelocity(JPH::BodyID id, glm::vec3 velocity);
		glm::vec3 GetLinearVelocity(JPH::BodyID id) const;
		void SetAngularVelocity(JPH::BodyID id, glm::vec3 velocity);
		glm::vec3 GetAngularVelocity(JPH::BodyID id) const;
		void AddImpulse(JPH::BodyID id, glm::vec3 impulse);
		void AddForce(JPH::BodyID id, glm::vec3 force);

		// Teleport a body (does not generate contacts for the move).
		void SetPosition(JPH::BodyID id, glm::vec3 position);
		void SetRotation(JPH::BodyID id, glm::quat rotation);

		// -- Raycasting ----------------------------------------------------------

		// Result of a single raycast query.
		struct RaycastResult
		{
			bool hit = false;                  // true if a surface was hit
			glm::vec3 position{0.f, 0.f, 0.f}; // world-space hit point
			glm::vec3 normal{0.f, 0.f, 0.f};   // surface normal at hit point
			float fraction = 1.f;              // hit distance / maxDistance
			std::uint32_t bodyId = 0;          // Jolt BodyID that was hit (0 = invalid)
		};

		// Cast a ray against the physics world.
		// direction must be normalized. maxDistance is the ray length.
		// Only collides with bodies on the specified layer.
		// Returns immediately with hit=false if no surface is found within maxDistance.
		RaycastResult CastRay(glm::vec3 origin, glm::vec3 direction, float maxDistance, PhysicsLayer layer = PhysicsLayer::NonMoving);

		// Cast a downward ray (direction = {0,-1,0}) from origin, find ground.
		// Convenience wrapper for IK foot planting. maxDistance is the maximum
		// trace height above the expected floor (e.g. step height).
		RaycastResult CastGround(glm::vec3 origin, float maxDistance = 2.0f)
		{
			return CastRay(origin, {0.f, -1.f, 0.f}, maxDistance, PhysicsLayer::NonMoving);
		}

		// Raw Jolt system - for advanced use (raycasts, queries, etc.).
		[[nodiscard]] JPH::PhysicsSystem& GetJoltSystem()
		{
			return *m_physics;
		}

		[[nodiscard]] const JPH::PhysicsSystem& GetJoltSystem() const
		{
			return *m_physics;
		}

		static constexpr float kFixedTimestep = 1.0f / 60.0f;

	private:
		// Consume pending *BodyDesc components and create Jolt bodies for them.
		// Called at the top of every Update before the physics step.
		void FlushPendingBodies(World& world);

		void StepPhysics();
		void SyncTransforms(World& world, float alpha);

		// Jolt internal implementations - defined in .cpp to keep header light.
		struct BPLayerInterface;
		struct ObjVsBPLayerFilter;
		struct ObjVsObjLayerFilter;

		std::unique_ptr<BPLayerInterface> m_bpLayerInterface;
		std::unique_ptr<ObjVsBPLayerFilter> m_objVsBPFilter;
		std::unique_ptr<ObjVsObjLayerFilter> m_objVsObjFilter;
		std::unique_ptr<JPH::TempAllocatorImpl> m_tempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool> m_jobSystem;
		std::unique_ptr<JPH::PhysicsSystem> m_physics;

		float m_accumulator = 0.0f;
		bool m_needsBroadPhaseOptimize = false;

		// Auto-disconnects in the destructor (declared last so it disconnects
		// before m_physics is destroyed - reverse member destruction order).
		entt::scoped_connection m_rigidBodyDestroyConn;
	};

} // namespace aether
