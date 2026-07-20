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

		void SetLinearVelocity(PhysicsBodyHandle body, glm::vec3 velocity);
		[[nodiscard]] glm::vec3 GetLinearVelocity(PhysicsBodyHandle body);
		void SetAngularVelocity(PhysicsBodyHandle body, glm::vec3 velocity);
		[[nodiscard]] glm::vec3 GetAngularVelocity(PhysicsBodyHandle body);
		void AddImpulse(PhysicsBodyHandle body, glm::vec3 impulse);
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

		// backing Jolt constraint so constraints never leak.
		void OnJointDestroyed(entt::registry& registry, entt::entity enttEntity);

	private:
		void FlushPendingBodies(World& world);
		void FlushPendingJoints(World& world);
		void RemoveJointConstraint(std::uint32_t constraintId);
		// Removes every joint constraint attached to (or targeting) enttEntity.
		void DestroyJointsTouching(entt::registry& registry, entt::entity enttEntity);
		void DrainContactEvents(World& world);

		void StepPhysics();
		void SyncTransforms(World& world, float alpha);

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
	};

} // namespace aether
