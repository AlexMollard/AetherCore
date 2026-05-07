#pragma once

#include <memory>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "System.hpp"
#include "Entity.hpp"
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

	// ── Shape descriptors ─────────────────────────────────────────────────────

	struct BoxBodySettings
	{
		glm::vec3         halfExtents{ 0.5f, 0.5f, 0.5f };
		PhysicsMotionType motionType  = PhysicsMotionType::Dynamic;
		PhysicsLayer      layer       = PhysicsLayer::Moving;
		float             friction    = 0.5f;
		float             restitution = 0.0f;
		bool              startActive = true;
	};

	struct SphereBodySettings
	{
		float             radius      = 0.5f;
		PhysicsMotionType motionType  = PhysicsMotionType::Dynamic;
		PhysicsLayer      layer       = PhysicsLayer::Moving;
		float             friction    = 0.5f;
		float             restitution = 0.0f;
		bool              startActive = true;
	};

	struct CapsuleBodySettings
	{
		float             halfHeight  = 0.5f;
		float             radius      = 0.25f;
		PhysicsMotionType motionType  = PhysicsMotionType::Dynamic;
		PhysicsLayer      layer       = PhysicsLayer::Moving;
		float             friction    = 0.5f;
		float             restitution = 0.0f;
		bool              startActive = true;
	};

	// ── PhysicsSystem ─────────────────────────────────────────────────────────
	//
	// Owns the Jolt physics world and drives it with a fixed timestep.
	// Using a fixed step (kFixedTimestep = 1/60 s) is essential for:
	//   - determinism across machines (required for lockstep networking)
	//   - stable simulation regardless of render framerate
	//
	// Usage:
	//   auto physics = std::make_unique<PhysicsSystem>();
	//   PhysicsSystem* physicsPtr = physics.get();
	//   world.RegisterSystem(std::move(physics));
	//   physicsPtr->AddBoxBody(world, entity, { .halfExtents = {1,1,1} });
	//   physicsPtr->OptimizeBroadPhase(); // call once after adding static bodies
	//
	class PhysicsSystem final : public System
	{
	public:
		PhysicsSystem();
		~PhysicsSystem() override;

		// System interface
		void       OnRegister(World& world) override;
		void       Update(World& world, float dt) override;
		void       OnUnregister(World& world) override;
		const char* GetName() const override { return "PhysicsSystem"; }

		// Call once after all static (NonMoving) bodies are added to accelerate
		// broadphase queries. Optional but recommended.
		void OptimizeBroadPhase();

		// ── Body factory ──────────────────────────────────────────────────────
		// Each function creates a Jolt body from the entity's current
		// TransformComponent position and adds RigidBodyComponent +
		// PhysicsStateComponent to the entity.

		void AddBoxBody   (World& world, Entity entity, BoxBodySettings    settings = {});
		void AddSphereBody(World& world, Entity entity, SphereBodySettings settings = {});
		void AddCapsuleBody(World& world, Entity entity, CapsuleBodySettings settings = {});

		// Remove the physics body associated with an entity and strip the
		// RigidBodyComponent / PhysicsStateComponent from it.
		void RemoveBody(World& world, Entity entity);

		// ── Body control ──────────────────────────────────────────────────────

		void      SetLinearVelocity (JPH::BodyID id, glm::vec3 velocity);
		glm::vec3 GetLinearVelocity (JPH::BodyID id) const;
		void      SetAngularVelocity(JPH::BodyID id, glm::vec3 velocity);
		glm::vec3 GetAngularVelocity(JPH::BodyID id) const;
		void      AddImpulse        (JPH::BodyID id, glm::vec3 impulse);
		void      AddForce          (JPH::BodyID id, glm::vec3 force);

		// Teleport a body (does not generate contacts for the move).
		void SetPosition(JPH::BodyID id, glm::vec3 position);
		void SetRotation(JPH::BodyID id, glm::quat rotation);

		// Raw Jolt system - for advanced use (raycasts, queries, etc.).
		[[nodiscard]] JPH::PhysicsSystem& GetJoltSystem() { return *m_physics; }
		[[nodiscard]] const JPH::PhysicsSystem& GetJoltSystem() const { return *m_physics; }

		// Physics timestep used for fixed-step integration.
		static constexpr float kFixedTimestep = 1.0f / 60.0f;

	private:
		void StepPhysics();
		void SyncTransforms(World& world, float alpha);

		// Jolt internal implementations - defined in .cpp to keep header light.
		struct BPLayerInterface;
		struct ObjVsBPLayerFilter;
		struct ObjVsObjLayerFilter;

		std::unique_ptr<BPLayerInterface>      m_bpLayerInterface;
		std::unique_ptr<ObjVsBPLayerFilter>    m_objVsBPFilter;
		std::unique_ptr<ObjVsObjLayerFilter>   m_objVsObjFilter;
		std::unique_ptr<JPH::TempAllocatorImpl>    m_tempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool>  m_jobSystem;
		std::unique_ptr<JPH::PhysicsSystem>        m_physics;

		float m_accumulator = 0.0f;
	};

} // namespace aether
