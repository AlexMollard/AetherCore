// Jolt must be the first include in every translation unit that uses it.
#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <thread>

#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{

	// -- Object / broadphase layer definitions ------------------------------------

	namespace Layers
	{
		static constexpr JPH::ObjectLayer kNonMoving = 0;
		static constexpr JPH::ObjectLayer kMoving = 1;
		static constexpr JPH::ObjectLayer kSensor = 2;
		static constexpr uint32_t kNumLayers = 3;
	} // namespace Layers

	namespace BroadPhaseLayers
	{
		static constexpr JPH::BroadPhaseLayer kNonMoving{0};
		static constexpr JPH::BroadPhaseLayer kMoving{1};
		static constexpr uint32_t kNumLayers = 2;
	} // namespace BroadPhaseLayers

	// -- PhysicsSystem internal Jolt interface implementations --------------------

	struct PhysicsSystem::BPLayerInterface final : public JPH::BroadPhaseLayerInterface
	{
		[[nodiscard]] uint32_t GetNumBroadPhaseLayers() const override
		{
			return BroadPhaseLayers::kNumLayers;
		}

		[[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
		{
			switch (layer)
			{
				case Layers::kNonMoving:
					return BroadPhaseLayers::kNonMoving;
				case Layers::kMoving:
					return BroadPhaseLayers::kMoving;
				case Layers::kSensor:
					return BroadPhaseLayers::kMoving;
				default:
					JPH_ASSERT(false);
					return BroadPhaseLayers::kNonMoving;
			}
		}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
		{
			switch ((JPH::BroadPhaseLayer::Type) layer)
			{
				case (JPH::BroadPhaseLayer::Type) BroadPhaseLayers::kNonMoving:
					return "NonMoving";
				case (JPH::BroadPhaseLayer::Type) BroadPhaseLayers::kMoving:
					return "Moving";
				default:
					return "Invalid";
			}
		}
#endif
	};

	struct PhysicsSystem::ObjVsBPLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
	{
		[[nodiscard]] bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer bp) const override
		{
			switch (object)
			{
				case Layers::kNonMoving:
					return bp == BroadPhaseLayers::kMoving;
				case Layers::kMoving:
					return true;
				case Layers::kSensor:
					return bp == BroadPhaseLayers::kMoving;
				default:
					return false;
			}
		}
	};

	struct PhysicsSystem::ObjVsObjLayerFilter final : public JPH::ObjectLayerPairFilter
	{
		[[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
		{
			switch (a)
			{
				case Layers::kNonMoving:
					return b == Layers::kMoving;
				case Layers::kMoving:
					return true; // Moving collides with everything
				case Layers::kSensor:
					return b == Layers::kMoving; // Sensors detect movers only
				default:
					return false;
			}
		}
	};

	// -- Helpers -------------------------------------------------------------------

	static JPH::ObjectLayer ToJoltLayer(PhysicsLayer layer)
	{
		switch (layer)
		{
			case PhysicsLayer::NonMoving:
				return Layers::kNonMoving;
			case PhysicsLayer::Moving:
				return Layers::kMoving;
			case PhysicsLayer::Sensor:
				return Layers::kSensor;
		}
		return Layers::kMoving;
	}

	static JPH::EMotionType ToJoltMotionType(PhysicsMotionType t)
	{
		switch (t)
		{
			case PhysicsMotionType::Static:
				return JPH::EMotionType::Static;
			case PhysicsMotionType::Kinematic:
				return JPH::EMotionType::Kinematic;
			case PhysicsMotionType::Dynamic:
				return JPH::EMotionType::Dynamic;
		}
		return JPH::EMotionType::Dynamic;
	}

	static glm::vec3 FromJolt(JPH::Vec3Arg v)
	{
		return {v.GetX(), v.GetY(), v.GetZ()};
	}

	static glm::quat FromJolt(JPH::QuatArg q)
	{
		return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()};
	}

	static JPH::Vec3 ToJolt(glm::vec3 v)
	{
		return {v.x, v.y, v.z};
	}

	static JPH::Quat ToJolt(glm::quat q)
	{
		return {q.x, q.y, q.z, q.w};
	}

	static glm::mat4 ToTransform(glm::vec3 pos, glm::quat rot, glm::vec3 scale = glm::vec3(1.f))
	{
		return glm::scale(glm::translate(glm::mat4(1.f), pos) * glm::mat4_cast(rot), scale);
	}

	// -- PhysicsSystem -------------------------------------------------------------

	PhysicsSystem::PhysicsSystem() = default;
	PhysicsSystem::~PhysicsSystem() = default;

	void PhysicsSystem::OnRegister(World& world)
	{
		AE_PROFILE_ZONE();
		JPH::RegisterDefaultAllocator();

		JPH::Factory::sInstance = new JPH::Factory();
		JPH::RegisterTypes();

		// 10 MB scratch for per-step allocations; does not persist between steps.
		m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10u * 1024u * 1024u);

		// One worker thread per logical CPU minus the calling thread.
		const int workerThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
		m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerThreads);

		m_bpLayerInterface = std::make_unique<BPLayerInterface>();
		m_objVsBPFilter = std::make_unique<ObjVsBPLayerFilter>();
		m_objVsObjFilter = std::make_unique<ObjVsObjLayerFilter>();

		m_physics = std::make_unique<JPH::PhysicsSystem>();
		m_physics->Init(
		        /*maxBodies*/ 65'536,
		        /*numBodyMutexes*/ 0, // 0 = auto
		        /*maxBodyPairs*/ 65'536,
		        /*maxContactConstraints*/ 10'240,
		        *m_bpLayerInterface,
		        *m_objVsBPFilter,
		        *m_objVsObjFilter);

		m_physics->SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));

		// Auto-cleanup: when an entity with RigidBodyComponent is destroyed, the
		// backing Jolt body is removed and freed so it doesn't leak into the next
		// scene load. This fires for every destruction path (world.Destroy(),
		// registry.remove<RigidBodyComponent>(), etc.).
		m_rigidBodyDestroyConn = world.GetRegistry().on_destroy<RigidBodyComponent>().connect<&PhysicsSystem::OnRigidBodyDestroyed>(this);

		AE_INFO(LogCategory::Engine, "PhysicsSystem initialised (Jolt, {} worker threads, fixed dt = {:.4f} s)", workerThreads, kFixedTimestep);
	}

	void PhysicsSystem::OnUnregister([[maybe_unused]] World& world)
	{
		AE_PROFILE_ZONE();
		m_physics.reset();
		m_jobSystem.reset();
		m_tempAllocator.reset();
		m_objVsObjFilter.reset();
		m_objVsBPFilter.reset();
		m_bpLayerInterface.reset();

		JPH::UnregisterTypes();
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
	}

	// -- Fixed-step update ---------------------------------------------------------

	void PhysicsSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		FlushPendingBodies(world);

		m_accumulator += dt;

		while (m_accumulator >= kFixedTimestep)
		{
			// Save previous state before stepping - used by SyncTransforms for interpolation.
			for (const auto& [entity, state]: world.View<PhysicsStateComponent>().each())
			{
				state.prevPosition = state.currPosition;
				state.prevRotation = state.currRotation;
			}

			StepPhysics();
			m_accumulator -= kFixedTimestep;
		}

		// alpha = how far into the next step we are; interpolate between prev/curr.
		const float alpha = m_accumulator / kFixedTimestep;
		SyncTransforms(world, alpha);
	}

	void PhysicsSystem::StepPhysics()
	{
		AE_PROFILE_ZONE();
		// collision_steps = 1 is fine for most games at 60 Hz.
		m_physics->Update(kFixedTimestep, /*collision_steps*/ 1, m_tempAllocator.get(), m_jobSystem.get());
	}

	void PhysicsSystem::SyncTransforms(World& world, float alpha)
	{
		AE_PROFILE_ZONE();
		auto& bodyInterface = m_physics->GetBodyInterface();

		for (const auto& [entity, rigid, state, transform]: world.View<RigidBodyComponent, PhysicsStateComponent, TransformComponent>().each())
		{
			if (rigid.bodyId.IsInvalid() || rigid.motionType == PhysicsMotionType::Static)
			{
				continue;
			}

			// Read current physics state.
			const auto pos = bodyInterface.GetCenterOfMassPosition(rigid.bodyId);
			const auto rot = bodyInterface.GetRotation(rigid.bodyId);
			state.currPosition = FromJolt(pos);
			state.currRotation = FromJolt(rot);

			// Interpolate for smooth rendering at any framerate.
			const glm::vec3 renderPos = glm::mix(state.prevPosition, state.currPosition, alpha);
			const glm::quat renderRot = glm::slerp(state.prevRotation, state.currRotation, alpha);

			transform.localToWorld = ToTransform(renderPos, renderRot, state.scale);
		}
	}

	// -- Body factory --------------------------------------------------------------

	static glm::vec3 ExtractPosition(const TransformComponent& t)
	{
		return {t.localToWorld[3][0], t.localToWorld[3][1], t.localToWorld[3][2]};
	}

	static glm::quat ExtractRotation(const TransformComponent& t)
	{
		// Extract upper-left 3x3, normalise out scale, then build quat.
		glm::mat3 rot(t.localToWorld);
		rot[0] = glm::normalize(rot[0]);
		rot[1] = glm::normalize(rot[1]);
		rot[2] = glm::normalize(rot[2]);
		return glm::quat_cast(rot);
	}

	static void AddBodyToEntity(World& world, Entity entity, JPH::BodyInterface& bodyInterface, JPH::BodyCreationSettings& settings, PhysicsMotionType motionType, glm::vec3 visualScale, bool startActive)
	{
		const JPH::Body* body = bodyInterface.CreateBody(settings);
		if (!body)
		{
			AE_WARN(LogCategory::Engine, "PhysicsSystem: failed to create body - max body limit reached");
			return;
		}

		const JPH::BodyID id = body->GetID();
		bodyInterface.AddBody(id, startActive ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

		world.Emplace<RigidBodyComponent>(entity, id, motionType);

		// Seed both prev and curr to current position so there's no initial interpolation pop.
		const glm::vec3 pos = FromJolt(bodyInterface.GetCenterOfMassPosition(id));
		const glm::quat rot = FromJolt(bodyInterface.GetRotation(id));
		world.Emplace<PhysicsStateComponent>(entity, pos, rot, pos, rot, visualScale);

		// Set the initial transform so the app never needs to bake scale manually.
		if (auto* tc = world.TryGet<TransformComponent>(entity))
		{
			tc->localToWorld = ToTransform(pos, rot, visualScale);
		}
	}

	void PhysicsSystem::FlushPendingBodies(World& world)
	{
		AE_PROFILE_ZONE();
		auto& bi = m_physics->GetBodyInterface();
		auto& reg = world.GetRegistry();
		bool addedStatic = false;

		// World::View returns raw entt views; entities come back as entt::entity.
		// aether::Entity and entt::entity share the same bit representation, so we
		// can reconstruct one from the other inline wherever World's typed API is needed.
		auto toAether = [](entt::entity e) -> Entity
		{
			return Entity{static_cast<uint32_t>(entt::to_integral(e))};
		};

		auto applyVelocity = [&](entt::entity e, glm::vec3 v)
		{
			if (glm::length(v) > 0.f)
			{
				if (const auto* r = reg.try_get<RigidBodyComponent>(e))
				{
					bi.SetLinearVelocity(r->bodyId, ToJolt(v));
				}
			}
		};

		// -- Box -------------------------------------------------------------------
		for (const auto& [entity, desc]: world.View<BoxBodyDesc>().each())
		{
			if (reg.any_of<RigidBodyComponent>(entity))
			{
				continue;
			}

			const auto* tc = reg.try_get<TransformComponent>(entity);
			JPH::BoxShapeSettings ss{ToJolt(desc.halfExtents)};
			ss.mMaterial = nullptr;
			auto result = ss.Create();
			if (result.HasError())
			{
				AE_WARN(LogCategory::Engine, "PhysicsSystem: box shape error: {}", result.GetError().c_str());
				continue;
			}

			const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
			const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);
			JPH::BodyCreationSettings bcs{result.Get(), JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), ToJoltMotionType(desc.motionType), ToJoltLayer(desc.layer)};
			bcs.mFriction = desc.friction;
			bcs.mRestitution = desc.restitution;

			AddBodyToEntity(world, toAether(entity), bi, bcs, desc.motionType, desc.halfExtents * 2.f, desc.startActive);
			if (desc.motionType == PhysicsMotionType::Static)
			{
				addedStatic = true;
			}
			applyVelocity(entity, desc.initialVelocity);
			reg.emplace<PhysicsDebugShapeComponent>(entity,
			        PhysicsDebugShapeComponent{
			                .shapeType = PhysicsShapeType::Box,
			                .halfExtents = desc.halfExtents,
			        });
		}
		for (auto entity: world.View<BoxBodyDesc>())
		{
			if (reg.any_of<RigidBodyComponent>(entity))
			{
				reg.remove<BoxBodyDesc>(entity);
			}
		}

		// -- Sphere ----------------------------------------------------------------
		for (const auto& [entity, desc]: world.View<SphereBodyDesc>().each())
		{
			if (reg.any_of<RigidBodyComponent>(entity))
			{
				continue;
			}

			const auto* tc = reg.try_get<TransformComponent>(entity);
			JPH::SphereShapeSettings ss{desc.radius};
			auto result = ss.Create();
			if (result.HasError())
			{
				AE_WARN(LogCategory::Engine, "PhysicsSystem: sphere shape error: {}", result.GetError().c_str());
				continue;
			}

			const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
			const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);
			JPH::BodyCreationSettings bcs{result.Get(), JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), ToJoltMotionType(desc.motionType), ToJoltLayer(desc.layer)};
			bcs.mFriction = desc.friction;
			bcs.mRestitution = desc.restitution;

			AddBodyToEntity(world, toAether(entity), bi, bcs, desc.motionType, glm::vec3(desc.radius * 2.f), desc.startActive);
			if (desc.motionType == PhysicsMotionType::Static)
			{
				addedStatic = true;
			}
			applyVelocity(entity, desc.initialVelocity);
			reg.emplace<PhysicsDebugShapeComponent>(entity,
			        PhysicsDebugShapeComponent{
			                .shapeType = PhysicsShapeType::Sphere,
			                .radius = desc.radius,
			        });
		}
		for (auto entity: world.View<SphereBodyDesc>())
		{
			if (reg.any_of<RigidBodyComponent>(entity))
			{
				reg.remove<SphereBodyDesc>(entity);
			}
		}

		// -- Capsule ---------------------------------------------------------------
		for (const auto& [entity, desc]: world.View<CapsuleBodyDesc>().each())
		{
			if (reg.any_of<RigidBodyComponent>(entity))
			{
				continue;
			}

			const auto* tc = reg.try_get<TransformComponent>(entity);
			JPH::CapsuleShapeSettings ss{desc.halfHeight, desc.radius};
			auto result = ss.Create();
			if (result.HasError())
			{
				AE_WARN(LogCategory::Engine, "PhysicsSystem: capsule shape error: {}", result.GetError().c_str());
				continue;
			}

			const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
			const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);
			JPH::BodyCreationSettings bcs{result.Get(), JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), ToJoltMotionType(desc.motionType), ToJoltLayer(desc.layer)};
			bcs.mFriction = desc.friction;
			bcs.mRestitution = desc.restitution;

			const glm::vec3 capsuleScale{desc.radius * 2.f, desc.halfHeight * 2.f + desc.radius * 2.f, desc.radius * 2.f};
			AddBodyToEntity(world, toAether(entity), bi, bcs, desc.motionType, capsuleScale, desc.startActive);
			if (desc.motionType == PhysicsMotionType::Static)
			{
				addedStatic = true;
			}
			applyVelocity(entity, desc.initialVelocity);
			reg.emplace<PhysicsDebugShapeComponent>(entity,
			        PhysicsDebugShapeComponent{
			                .shapeType = PhysicsShapeType::Capsule,
			                .radius = desc.radius,
			                .halfHeight = desc.halfHeight,
			        });
		}
		for (auto entity: world.View<CapsuleBodyDesc>())
		{
			if (reg.any_of<RigidBodyComponent>(entity))
			{
				reg.remove<CapsuleBodyDesc>(entity);
			}
		}

		if (addedStatic)
		{
			m_physics->OptimizeBroadPhase();
		}
	}

	void PhysicsSystem::RemoveBody(World& world, Entity entity)
	{
		auto* rigid = world.TryGet<RigidBodyComponent>(entity);
		if (!rigid || rigid->bodyId.IsInvalid())
		{
			return;
		}

		auto& bodyInterface = m_physics->GetBodyInterface();
		bodyInterface.RemoveBody(rigid->bodyId);
		bodyInterface.DestroyBody(rigid->bodyId);

		world.Remove<RigidBodyComponent>(entity);
		world.Remove<PhysicsStateComponent>(entity);
		world.Remove<PhysicsDebugShapeComponent>(entity);
	}

	void PhysicsSystem::OnRigidBodyDestroyed(entt::registry& registry, entt::entity enttEntity)
	{
		if (!m_physics)
		{
			return;
		}

		auto* rigid = registry.try_get<RigidBodyComponent>(enttEntity);
		if (!rigid || rigid->bodyId.IsInvalid())
		{
			return;
		}

		auto& bodyInterface = m_physics->GetBodyInterface();
		bodyInterface.RemoveBody(rigid->bodyId);
		bodyInterface.DestroyBody(rigid->bodyId);
	}

	// -- Body control --------------------------------------------------------------

	void PhysicsSystem::SetLinearVelocity(JPH::BodyID id, glm::vec3 v)
	{
		m_physics->GetBodyInterface().SetLinearVelocity(id, ToJolt(v));
	}

	glm::vec3 PhysicsSystem::GetLinearVelocity(JPH::BodyID id) const
	{
		return FromJolt(m_physics->GetBodyInterface().GetLinearVelocity(id));
	}

	void PhysicsSystem::SetAngularVelocity(JPH::BodyID id, glm::vec3 v)
	{
		m_physics->GetBodyInterface().SetAngularVelocity(id, ToJolt(v));
	}

	glm::vec3 PhysicsSystem::GetAngularVelocity(JPH::BodyID id) const
	{
		return FromJolt(m_physics->GetBodyInterface().GetAngularVelocity(id));
	}

	void PhysicsSystem::AddImpulse(JPH::BodyID id, glm::vec3 impulse)
	{
		m_physics->GetBodyInterface().AddImpulse(id, ToJolt(impulse));
	}

	void PhysicsSystem::AddForce(JPH::BodyID id, glm::vec3 force)
	{
		m_physics->GetBodyInterface().AddForce(id, ToJolt(force));
	}

	void PhysicsSystem::SetPosition(JPH::BodyID id, glm::vec3 pos)
	{
		m_physics->GetBodyInterface().SetPosition(id, JPH::RVec3(pos.x, pos.y, pos.z), JPH::EActivation::Activate);
	}

	void PhysicsSystem::SetRotation(JPH::BodyID id, glm::quat rot)
	{
		m_physics->GetBodyInterface().SetRotation(id, ToJolt(rot), JPH::EActivation::Activate);
	}

	// -- Raycasting --------------------------------------------------------------

	PhysicsSystem::RaycastResult PhysicsSystem::CastRay(glm::vec3 origin, glm::vec3 direction, float maxDistance, PhysicsLayer /*layer*/)
	{
		RaycastResult result{};

		if (maxDistance <= 0.0f || glm::length(direction) < 0.001f)
		{
			return result;
		}

		direction = glm::normalize(direction);

		// RRayCast: RVec3 (double) origin, Vec3 (float) direction+length.
		// The direction vector's length IS the maxDistance in Jolt's convention.
		JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), maxDistance * JPH::Vec3(direction.x, direction.y, direction.z));

		JPH::RayCastResult joltResult;

		m_physics->GetNarrowPhaseQuery().CastRay(ray, joltResult, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{}, JPH::BodyFilter{});

		if (!joltResult.mBodyID.IsInvalid())
		{
			result.hit = true;
			result.bodyId = static_cast<std::uint32_t>(joltResult.mBodyID.GetIndex());
			result.fraction = joltResult.mFraction;

			// Hit position: mOrigin + fraction * mDirection (both in RVec3/double).
			JPH::RVec3 hitPosR = ray.GetPointOnRay(joltResult.mFraction);
			result.position = {hitPosR.GetX(), hitPosR.GetY(), hitPosR.GetZ()};

			// Normal: approximate as world-up for ground detection.
			// For true surface normal a shape-level CastRay would be needed.
			result.normal = {0.0f, 1.0f, 0.0f};
		}

		return result;
	}

} // namespace aether
