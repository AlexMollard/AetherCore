#pragma once

#include <atomic>
#include <memory>
#include <semaphore>
#include <thread>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <entt/entt.hpp>

#include "scene/System.hpp"
#include "scene/Entity.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether
{
	class World;

	// -- PhysicsSystem ---------------------------------------------------------
	//
	// Owns the Jolt physics world and drives it with a fixed timestep on a
	// dedicated background thread.
	// Using a fixed step (kFixedTimestep = 1/60 s) is essential for:
	//   - determinism across machines (required for lockstep networking)
	//   - stable simulation regardless of render framerate
	//
	// Threading model (pipelined, 1 frame of latency — same pattern as RenderThread):
	//   Game thread:   FlushPendingBodies → Kick step N → [other systems overlap] → ...
	//   Physics thread:                      Run step N → Done
	//   Game thread (next frame): Wait for step N → SyncTransforms(N-1) → ...
	//
	// All public methods that touch the physics body interface call WaitForStep()
	// internally, so the first physics API call after a kick becomes the sync
	// point.  This gives maximum overlap when no physics API is called during the
	// overlap window, and safe serialisation when one is.
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
	//   // physics body, sets the correct scaled transform, and removes the descriptor.
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

		[[nodiscard]] const char* GetName() const override
		{
			return "PhysicsSystem";
		}

		// Remove the physics body associated with an entity and strip the
		// RigidBodyComponent / PhysicsStateComponent from it.
		void RemoveBody(World& world, Entity entity);

		// Called via entt sink when a RigidBodyComponent is destroyed.
		// Removes and destroys the backing physics body so we never leak physics
		// bodies when entities are destroyed outside of RemoveBody().
		void OnRigidBodyDestroyed(entt::registry& registry, entt::entity enttEntity);

		// -- Body control ------------------------------------------------------

		void SetLinearVelocity(PhysicsBodyHandle body, glm::vec3 velocity);
		[[nodiscard]] glm::vec3 GetLinearVelocity(PhysicsBodyHandle body);
		void SetAngularVelocity(PhysicsBodyHandle body, glm::vec3 velocity);
		[[nodiscard]] glm::vec3 GetAngularVelocity(PhysicsBodyHandle body);
		void AddImpulse(PhysicsBodyHandle body, glm::vec3 impulse);
		void AddForce(PhysicsBodyHandle body, glm::vec3 force);

		// Teleport a body (does not generate contacts for the move).
		void SetPosition(PhysicsBodyHandle body, glm::vec3 position);
		void SetRotation(PhysicsBodyHandle body, glm::quat rotation);

		// Block until no async physics step is in flight. Callers that destroy
		// bodies outside Update (scene replace-all, editor stop-restore) MUST
		// call this first - Jolt body removal must not race the stepping thread.
		void WaitForStepIdle();

		// Editor-paused variant of Update: consume pending *BodyDesc components
		// into live bodies (so loaded/created entities are pickable and
		// teleportable) without stepping the simulation.
		void FlushPendingOnly(World& world);

		// -- Raycasting ----------------------------------------------------------

		// Result of a single raycast query.
		struct RaycastResult
		{
			bool hit = false;                  // true if a surface was hit
			glm::vec3 position{0.f, 0.f, 0.f}; // world-space hit point
			glm::vec3 normal{0.f, 0.f, 0.f};   // surface normal at hit point
			float fraction = 1.f;              // hit distance / maxDistance
			PhysicsBodyHandle body;            // Body that was hit
		};

		// Cast a ray against the physics world.
		// direction must be normalized. maxDistance is the ray length.
		// Only collides with bodies on the specified layer.
		// Returns immediately with hit=false if no surface is found within maxDistance.
		RaycastResult CastRay(glm::vec3 origin, glm::vec3 direction, float maxDistance);

		// Cast a downward ray (direction = {0,-1,0}) from origin, find ground.
		// Convenience wrapper for IK foot planting. maxDistance is the maximum
		// trace height above the expected floor (e.g. step height).
		RaycastResult CastGround(glm::vec3 origin, float maxDistance = 2.0f)
		{
			return CastRay(origin, {0.f, -1.f, 0.f}, maxDistance);
		}

		static constexpr float kFixedTimestep = 1.0f / 60.0f;

	private:
		// Consume pending *BodyDesc components and create Jolt bodies for them.
		// Called at the top of every Update before the physics step.
		void FlushPendingBodies(World& world);

		void StepPhysics();
		void SyncTransforms(World& world, float alpha);

		// Save prev = curr for all PhysicsStateComponents (called between steps).
		void SavePrevState(World& world);

		// -- Dedicated physics thread ------------------------------------------------

		// Thread entry point: waits on m_stepKick, runs StepPhysics, signals m_stepDone.
		void PhysicsThreadLoop();

		// Block until the in-flight step (if any) completes.  Called by Update
		// at the top of each frame and by every public Jolt-touching method so
		// the first API call after a kick becomes the natural sync point.
		void WaitForStep();

		void StartPhysicsThread();
		void StopPhysicsThread();

		struct Impl;
		std::unique_ptr<Impl> m_impl;

		float m_accumulator = 0.0f;

		// Dedicated physics thread — runs m_physics->Update() off the game thread.
		// Ping-pong semaphores: Kick signals "start a step", Done signals "step finished".
		std::thread m_physicsThread;
		std::binary_semaphore m_stepKick{0};
		std::binary_semaphore m_stepDone{0};
		std::atomic<bool> m_physicsThreadRunning{false};

		// Game-thread-only flag: true between Kick and Wait.  Not atomic — only
		// ever read/written on the game thread.
		bool m_stepInFlight = false;

		// Alpha saved at end of each Update for the next frame's SyncTransforms.
		float m_lastAlpha = 0.0f;

		// Auto-disconnects in the destructor (declared last so it disconnects
		// before m_impl is destroyed - reverse member destruction order).
		entt::scoped_connection m_rigidBodyDestroyConn;
	};

} // namespace aether
