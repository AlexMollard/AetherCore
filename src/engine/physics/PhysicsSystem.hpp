#pragma once

#include <atomic>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <entt/entt.hpp>

#include "scene/System.hpp"
#include "scene/Entity.hpp"
#include "physics/PhysicsComponents.hpp"

namespace aether
{
	class World;

	// dedicated background thread.
	class PhysicsSystem final : public System
	{
	public:
		PhysicsSystem();
		~PhysicsSystem() override;
		PhysicsSystem(const PhysicsSystem&) = delete;
		PhysicsSystem& operator=(const PhysicsSystem&) = delete;
		PhysicsSystem(PhysicsSystem&&) = delete;
		PhysicsSystem& operator=(PhysicsSystem&&) = delete;

		void OnRegister(World& world) override;
		void Update(World& world, float dt) override;
		void OnUnregister(World& world) override;

		[[nodiscard]] const char* GetName() const override
		{
			return "PhysicsSystem";
		}

		void RemoveBody(World& world, Entity entity);

		// Removes and destroys the backing physics body so we never leak physics
		void OnRigidBodyDestroyed(entt::registry& registry, entt::entity enttEntity);

		// Removes and destroys the backing JPH::CharacterVirtual so we never leak one.
		void OnCharacterControllerDestroyed(entt::registry& registry, entt::entity enttEntity);

		void SetLinearVelocity(PhysicsBodyHandle body, glm::vec3 velocity);
		[[nodiscard]] glm::vec3 GetLinearVelocity(PhysicsBodyHandle body);
		void SetAngularVelocity(PhysicsBodyHandle body, glm::vec3 velocity);
		[[nodiscard]] glm::vec3 GetAngularVelocity(PhysicsBodyHandle body);
		void AddImpulse(PhysicsBodyHandle body, glm::vec3 impulse);
		// Applied at a world-space point rather than the centre of mass, so it imparts
		// spin (a thrown prop tumbles) exactly like a real hit or grab-and-throw would.
		void AddImpulseAtPoint(PhysicsBodyHandle body, glm::vec3 impulse, glm::vec3 worldPoint);
		void AddForce(PhysicsBodyHandle body, glm::vec3 force);
		void AddTorque(PhysicsBodyHandle body, glm::vec3 torque);
		void AddAngularImpulse(PhysicsBodyHandle body, glm::vec3 angularImpulse);

		void SetPosition(PhysicsBodyHandle body, glm::vec3 position);
		void SetRotation(PhysicsBodyHandle body, glm::quat rotation);

		void SetFriction(PhysicsBodyHandle body, float friction);
		void SetRestitution(PhysicsBodyHandle body, float restitution);
		void SetGravityFactor(PhysicsBodyHandle body, float factor);

		void SetBodyActive(PhysicsBodyHandle body, bool active);
		[[nodiscard]] bool IsBodyActive(PhysicsBodyHandle body);

		void RebuildBody(World& world, Entity entity);

		void RebuildJoint(World& world, Entity entity);

		// Changes an EXISTING body's motion type in place (Jolt's own
		// BodyInterface::SetMotionType - no destroy/rebuild needed, unlike Box2D's
		// RigidBody2DComponent path). Also updates RigidBodyComponent::motionType so
		// PushKinematicTargets/SyncTransforms agree with Jolt about what this body
		// now is. Used by NetworkContext::SyncSimulationAuthority to force a
		// non-owned Dynamic body Kinematic (and back) without touching its shape,
		// joints, or user data.
		void SetBodyMotionType(World& world, Entity entity, PhysicsMotionType motionType);

		// bodies outside Update (scene replace-all, editor stop-restore) MUST
		void WaitForStepIdle();

		void FlushPendingOnly(World& world);

		struct RaycastResult
		{
			bool hit = false;
			glm::vec3 position{0.f, 0.f, 0.f};
			glm::vec3 normal{0.f, 0.f, 0.f};
			float fraction = 1.f;
			PhysicsBodyHandle body;
			std::uint32_t entity = 0;
		};

		// direction must be normalized. maxDistance is the ray length.
		RaycastResult CastRay(glm::vec3 origin, glm::vec3 direction, float maxDistance);

		RaycastResult CastGround(glm::vec3 origin, float maxDistance = 2.0f)
		{
			return CastRay(origin, {0.f, -1.f, 0.f}, maxDistance);
		}

		RaycastResult SphereCast(glm::vec3 origin, glm::vec3 direction, float radius, float maxDistance);

		[[nodiscard]] std::vector<std::uint32_t> OverlapSphere(glm::vec3 center, float radius);

		static constexpr float kFixedTimestep = 1.0f / 60.0f;

		// -- Script/tool-gun constraints (ScriptJointsComponent) --------------------
		// Weld self rigidly to target (or to the world if target is invalid) at their
		// current relative pose - Jolt's FixedConstraintSettings auto-detects the
		// joint point, so no anchor is needed. Returns a handle immediately, valid to
		// pass to DestroyConstraint even before the next physics tick actually builds
		// the Jolt constraint (see JointEntry's own comment).
		std::uint32_t CreateFixedConstraint(World& world, Entity self, Entity target);

		// Ropes self to target (or to the world) at worldAnchor, holding them at
		// exactly restLength apart once built - a rigid rope, not an elastic one,
		// matching JointComponent's own Distance joint (equal min/max). restLength
		// must be >= 0; there is no "use current separation" option here the way
		// JointComponent's negative default gives the Inspector, because a script
		// caller has no "current separation" to fall back to meaningfully - it always
		// means something specific to a tool.
		std::uint32_t CreateDistanceConstraint(World& world, Entity self, Entity target, glm::vec3 worldAnchor, float restLength);

		// No-op on an already-destroyed or unknown handle - a script that destroys a
		// constraint whose OTHER endpoint died first (already cleaned up by
		// DestroyJointsTouching) must not have to check for that itself.
		void DestroyConstraint(World& world, std::uint32_t handle);

		// Mints a fresh handle and appends `entry` to self's ScriptJointsComponent
		// (creating it if absent) - the shared primitive behind CreateFixedConstraint/
		// CreateDistanceConstraint AND the scene serde's apply path (PhysicsSerde.cpp),
		// which restores a saved contraption's constraints this same way rather than
		// re-deriving handle bookkeeping a second time. `entry.handle`/`entry.created`
		// are overwritten regardless of what the caller set them to - callers describe
		// WHAT joint to add, never its runtime bookkeeping.
		std::uint32_t AddScriptJoint(World& world, Entity self, JointEntry entry);

		// backing Jolt constraint so constraints never leak.
		void OnJointDestroyed(entt::registry& registry, entt::entity enttEntity);

		// Same cleanup as OnJointDestroyed, for ScriptJointsComponent's OWN entries -
		// see DestroyJointsTouching's own comment for why this is a second connection
		// rather than folding the two components into DestroyJointsTouching's single
		// existing hook: they're independent lists that can each be non-empty on the
		// same entity.
		void OnScriptJointsDestroyed(entt::registry& registry, entt::entity enttEntity);

	private:
		void FlushPendingBodies(World& world);
		void FlushPendingJoints(World& world);
		void FlushPendingScriptJoints(World& world);
		void RemoveJointConstraint(std::uint32_t constraintId);
		// Removes every joint constraint attached to (or targeting) enttEntity.
		void DestroyJointsTouching(entt::registry& registry, entt::entity enttEntity);

		// Creates newly-added JPH::CharacterVirtual instances and refreshes the
		// physics-thread-owned input/output snapshot (Impl::LiveCharacter::io) for
		// existing ones. Game-thread only, called while the physics thread is idle -
		// see StepCharacters for why the two never touch the same state concurrently.
		void FlushPendingCharacters(World& world);
		// Physics-thread only, once per fixed substep, before the Jolt body simulation
		// step (see StepPhysics). Deliberately takes no World&: it reads/writes only
		// Impl::LiveCharacter::io, which FlushPendingCharacters/SyncTransforms
		// exchange with the ECS while the physics thread is guaranteed idle. Touching
		// entt from here would race the game thread, which keeps running other
		// systems against the same World while this frame's kicked step is in flight.
		void StepCharacters(float dt);
		void DrainContactEvents(World& world);

		void StepPhysics();
		void SyncTransforms(World& world, float alpha);
		// Scripts/network replication own a Kinematic body's transform: pushes each
		// one's CURRENT TransformComponent into its Jolt body before the physics step,
		// mirroring Physics2DSystem::PushKinematicTargets. Without this, a Kinematic
		// body only ever holds the position it was created with - SyncTransforms
		// skips it (see the .cpp), and nothing else ever tells Jolt where it moved to.
		void PushKinematicTargets(World& world);

		void SavePrevState(World& world);

		// -- Dedicated physics thread ------------------------------------------------

		// Thread entry point: waits on m_stepKick, runs StepPhysics, signals m_stepDone.
		void PhysicsThreadLoop();

		void WaitForStep();

		void StartPhysicsThread();
		void StopPhysicsThread();

		struct Impl;
		std::unique_ptr<Impl> m_impl;

		float m_accumulator = 0.0f;

		// Dedicated physics thread - runs m_physics->Update() off the game thread.
		std::thread m_physicsThread;
		std::binary_semaphore m_stepKick{0};
		std::binary_semaphore m_stepDone{0};
		std::atomic<bool> m_physicsThreadRunning{false};

		// Game-thread-only flag: true between Kick and Wait.  Not atomic - only
		bool m_stepInFlight = false;

		float m_lastAlpha = 0.0f;

		entt::scoped_connection m_rigidBodyDestroyConn;
		entt::scoped_connection m_jointDestroyConn;
		entt::scoped_connection m_scriptJointsDestroyConn;
		entt::scoped_connection m_characterDestroyConn;
	};

} // namespace aether
