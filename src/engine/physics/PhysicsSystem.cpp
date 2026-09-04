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
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Platform thread configuration (name / priority for the physics thread).
#if defined(_WIN32)
#	include <windows.h>
#elif defined(__linux__)
#	include <pthread.h>
#endif

#include "physics/PhysicsSystem.hpp"
#include "physics2d/PhysicsDomainGate.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{

	namespace Layers
	{
		static constexpr JPH::ObjectLayer kNonMoving = 0;
		static constexpr JPH::ObjectLayer kMoving = 1;
		static constexpr JPH::ObjectLayer kSensor = 2;
		static constexpr uint32_t kNumLayers = 3;
		// Nothing consults the count at runtime - Jolt only asks for the BROAD PHASE layer count - so
		// make it earn its keep as a compile-time check instead. Adding a layer without bumping this
		// now fails the build rather than leaving a stale number sitting next to the ids.
		static_assert(kSensor + 1u == kNumLayers, "kNumLayers must be one past the last object layer");
	} // namespace Layers

	namespace BroadPhaseLayers
	{
		static constexpr JPH::BroadPhaseLayer kNonMoving{0};
		static constexpr JPH::BroadPhaseLayer kMoving{1};
		static constexpr uint32_t kNumLayers = 2;
	} // namespace BroadPhaseLayers

	struct BPLayerInterface final : public JPH::BroadPhaseLayerInterface
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

	struct ObjVsBPLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
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

	struct ObjVsObjLayerFilter final : public JPH::ObjectLayerPairFilter
	{
		[[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
		{
			switch (a)
			{
				case Layers::kNonMoving:
					return b == Layers::kMoving;
				case Layers::kMoving:
					return true;
				case Layers::kSensor:
					return b == Layers::kMoving;
				default:
					return false;
			}
		}
	};

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

	static JPH::BodyID ToJolt(PhysicsBodyHandle handle)
	{
		return handle.IsValid() ? JPH::BodyID(handle.value) : JPH::BodyID();
	}

	static PhysicsBodyHandle FromJolt(JPH::BodyID id)
	{
		return id.IsInvalid() ? PhysicsBodyHandle{} : PhysicsBodyHandle{id.GetIndexAndSequenceNumber()};
	}

	static glm::mat4 ToTransform(glm::vec3 pos, glm::quat rot, glm::vec3 scale = glm::vec3(1.f))
	{
		return glm::scale(glm::translate(glm::mat4(1.f), pos) * glm::mat4_cast(rot), scale);
	}

	// with numerically-equal dimension packing never alias in the shared cache.

	static uint64_t BoxKey(glm::vec3 half)
	{
		const auto x = glm::packHalf1x16(half.x);
		const auto y = glm::packHalf1x16(half.y);
		const auto z = glm::packHalf1x16(half.z);
		return (uint64_t(x) << 32) | (uint64_t(y) << 16) | uint64_t(z) | (uint64_t(0) << 61);
	}

	static uint64_t SphereKey(float radius)
	{
		return uint64_t(glm::packHalf1x16(radius)) | (uint64_t(1) << 61);
	}

	static uint64_t CapsuleKey(float halfHeight, float radius)
	{
		const auto h = glm::packHalf1x16(halfHeight);
		const auto r = glm::packHalf1x16(radius);
		return (uint64_t(h) << 16) | uint64_t(r) | (uint64_t(2) << 61);
	}

	static uint64_t CylinderKey(float halfHeight, float radius)
	{
		const auto h = glm::packHalf1x16(halfHeight);
		const auto r = glm::packHalf1x16(radius);
		return (uint64_t(h) << 16) | uint64_t(r) | (uint64_t(3) << 61);
	}

	class JoltRuntime final
	{
	public:
		JoltRuntime()
		{
			const std::lock_guard lock(s_mutex);
			if (s_refCount++ == 0)
			{
				JPH::RegisterDefaultAllocator();
				JPH::Factory::sInstance = new JPH::Factory();
				JPH::RegisterTypes();
			}
		}

		~JoltRuntime()
		{
			const std::lock_guard lock(s_mutex);
			--s_refCount;
			if (s_refCount == 0)
			{
				JPH::UnregisterTypes();
				delete JPH::Factory::sInstance;
				JPH::Factory::sInstance = nullptr;
			}
		}

		JoltRuntime(const JoltRuntime&) = delete;
		JoltRuntime& operator=(const JoltRuntime&) = delete;

	private:
		static inline std::mutex s_mutex;
		static inline uint32_t s_refCount = 0;
	};

	// Buffers contact events off the physics thread (Jolt calls these from several
	class ContactCollector final : public JPH::ContactListener
	{
	public:
		struct Added
		{
			std::uint64_t entity1;
			std::uint64_t entity2;
			bool sensor;
		};

		struct Removed
		{
			JPH::BodyID body1;
			JPH::BodyID body2;
		};

		void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold&, JPH::ContactSettings&) override
		{
			const std::lock_guard lock(m_mutex);
			m_added.push_back({body1.GetUserData(), body2.GetUserData(), body1.IsSensor() || body2.IsSensor()});
		}

		void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
		{
			const std::lock_guard lock(m_mutex);
			m_removed.push_back({pair.GetBody1ID(), pair.GetBody2ID()});
		}

		// Hand the buffered events to the game thread and reset for the next step.
		void Take(std::vector<Added>& added, std::vector<Removed>& removed)
		{
			const std::lock_guard lock(m_mutex);
			added.swap(m_added);
			removed.swap(m_removed);
			m_added.clear();
			m_removed.clear();
		}

	private:
		std::mutex m_mutex;
		std::vector<Added> m_added;
		std::vector<Removed> m_removed;
	};

	struct PhysicsSystem::Impl
	{
		std::unique_ptr<JoltRuntime> runtime;
		std::unique_ptr<BPLayerInterface> bpLayerInterface;
		std::unique_ptr<ObjVsBPLayerFilter> objVsBPFilter;
		std::unique_ptr<ObjVsObjLayerFilter> objVsObjFilter;
		std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
		std::unique_ptr<JPH::PhysicsSystem> physics;
		std::unique_ptr<ContactCollector> contactCollector;
		std::unordered_map<uint64_t, JPH::ShapeRefC> shapeCache;

		JPH::Ref<JPH::GroupFilterTable> groupFilter;

		struct LiveConstraint
		{
			JPH::Ref<JPH::Constraint> constraint;
			std::uint32_t bodyA = 0;
			std::uint32_t bodyB = 0;
			bool collisionDisabled = false;
		};

		std::unordered_map<std::uint32_t, LiveConstraint> constraints;
		std::uint32_t nextConstraintId = 1;

		// Bodies pulled out of the simulation because their hierarchy was disabled,
		// keyed by entity id; SyncTransforms re-activates exactly these on re-enable.
		std::unordered_set<std::uint32_t> suspendedByDisable;

		std::vector<ContactCollector::Added> addedScratch;
		std::vector<ContactCollector::Removed> removedScratch;
	};

	PhysicsSystem::PhysicsSystem()
	      : m_impl(std::make_unique<Impl>())
	{
	}

	PhysicsSystem::~PhysicsSystem()
	{
		StopPhysicsThread();
	}

	// -- Dedicated physics thread -------------------------------------------------

	void PhysicsSystem::StartPhysicsThread()
	{
		m_physicsThreadRunning.store(true, std::memory_order_release);
		m_physicsThread = std::thread([this] { PhysicsThreadLoop(); });
	}

	void PhysicsSystem::StopPhysicsThread()
	{
		WaitForStep();
		m_physicsThreadRunning.store(false, std::memory_order_release);
		m_stepKick.release(); // wake the thread if it's blocked on the semaphore

		if (m_physicsThread.joinable())
		{
			m_physicsThread.join();
		}
	}

	void PhysicsSystem::PhysicsThreadLoop()
	{
		AE_PROFILE_THREAD("PhysicsThread");

#if defined(_WIN32)
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
		SetThreadDescription(GetCurrentThread(), L"AetherCore PhysicsThread");
#elif defined(__linux__)
		pthread_setname_np(pthread_self(), "Aether-Physics");
#endif

		while (m_physicsThreadRunning.load(std::memory_order_acquire))
		{
			// Block until the game thread kicks a step.
			{
				AE_PROFILE_ZONE_N("Phys.Thread.WaitKick");
				m_stepKick.acquire();
			}
			if (!m_physicsThreadRunning.load(std::memory_order_acquire))
			{
				break;
			}

			StepPhysics();
			m_stepDone.release();
		}
	}

	void PhysicsSystem::WaitForStep()
	{
		if (m_stepInFlight)
		{
			AE_PROFILE_ZONE_N("Phys.WaitForStep");
			m_stepDone.acquire();
			m_stepInFlight = false;
		}
	}

	void PhysicsSystem::SavePrevState(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.SavePrevState");
		for (const auto& [entity, state]: world.View<PhysicsStateComponent>().each())
		{
			state.prevPosition = state.currPosition;
			state.prevRotation = state.currRotation;
		}
	}

	void PhysicsSystem::OnRegister(World& world)
	{
		AE_PROFILE_ZONE();
		m_impl->runtime = std::make_unique<JoltRuntime>();

		m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10u * 1024u * 1024u);

		// One worker thread per logical CPU minus the calling thread and the
		const int workerThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
		m_impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerThreads);

		m_impl->bpLayerInterface = std::make_unique<BPLayerInterface>();
		m_impl->objVsBPFilter = std::make_unique<ObjVsBPLayerFilter>();
		m_impl->objVsObjFilter = std::make_unique<ObjVsObjLayerFilter>();

		m_impl->physics = std::make_unique<JPH::PhysicsSystem>();
		m_impl->physics->Init(
		        /*maxBodies*/ 64'536,
		        /*numBodyMutexes*/ 0,
		        /*maxBodyPairs*/ 64'536,
		        /*maxContactConstraints*/ 10'240,
		        *m_impl->bpLayerInterface,
		        *m_impl->objVsBPFilter,
		        *m_impl->objVsObjFilter);

		m_impl->physics->SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));

		m_impl->contactCollector = std::make_unique<ContactCollector>();
		m_impl->physics->SetContactListener(m_impl->contactCollector.get());

		m_impl->groupFilter = new JPH::GroupFilterTable(64'536);

		m_rigidBodyDestroyConn = world.GetRegistry().on_destroy<RigidBodyComponent>().connect<&PhysicsSystem::OnRigidBodyDestroyed>(this);
		m_jointDestroyConn = world.GetRegistry().on_destroy<JointComponent>().connect<&PhysicsSystem::OnJointDestroyed>(this);

		AE_INFO(LogCategory::Engine, "PhysicsSystem initialised (Jolt, {} worker threads, fixed dt = {:.4f} s, dedicated physics thread)", workerThreads, kFixedTimestep);

		StartPhysicsThread();
	}

	void PhysicsSystem::OnUnregister([[maybe_unused]] World& world)
	{
		AE_PROFILE_ZONE();
		StopPhysicsThread();

		m_impl->constraints.clear();
		m_impl->groupFilter = nullptr;
		m_impl->physics.reset();
		m_impl->contactCollector.reset();
		m_impl->shapeCache.clear();
		m_impl->jobSystem.reset();
		m_impl->tempAllocator.reset();
		m_impl->objVsObjFilter.reset();
		m_impl->objVsBPFilter.reset();
		m_impl->bpLayerInterface.reset();
		m_impl->runtime.reset();
	}

	void PhysicsSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();

		WaitForStep();

		SyncTransforms(world, m_lastAlpha);

		DrainContactEvents(world);

		FlushPendingBodies(world);
		FlushPendingJoints(world);

		// 4. Accumulate time and kick step(s) to the physics thread.
		m_accumulator += dt;

		// Spiral-of-death guard (mirrors Physics2DSystem): at elevated time-scale
		// (Play speed / backtick turbo) or after a long stall, dt can be worth many
		// fixed steps. Cap the catch-up and drop the excess debt so a single frame
		// never wedges on dozens of blocking WaitForStep()s.
		constexpr int kMaxStepsPerFrame = 8;
		int stepsThisFrame = 0;
		while (m_accumulator >= kFixedTimestep && stepsThisFrame < kMaxStepsPerFrame)
		{
			if (m_stepInFlight)
			{
				WaitForStep();
			}

			SavePrevState(world);
			m_accumulator -= kFixedTimestep;

			// Kick the step to the physics thread.  The last step in the loop
			m_stepKick.release();
			m_stepInFlight = true;
			++stepsThisFrame;
		}

		if (stepsThisFrame == kMaxStepsPerFrame && m_accumulator >= kFixedTimestep)
		{
			AE_WARN(LogCategory::Engine, "Physics dropped {:.1f} ms of simulation debt after {} steps in one frame", m_accumulator * 1000.0f, stepsThisFrame);
			m_accumulator = 0.0f;
		}

		m_lastAlpha = m_accumulator / kFixedTimestep;

		AE_PROFILE_PLOT("Phys.StepsPerFrame", static_cast<int64_t>(stepsThisFrame));
		AE_PROFILE_PLOT("Phys.AccumulatorMs", static_cast<int64_t>(m_accumulator * 1000.0f));
		AE_PROFILE_PLOT("Phys.RigidBodyCount", static_cast<int64_t>(world.View<RigidBodyComponent>().size()));
	}

	void PhysicsSystem::StepPhysics()
	{
		AE_PROFILE_ZONE_N("Phys.Step");
		AE_PROFILE_PLOT("Phys.TotalBodies", static_cast<int64_t>(m_impl->physics->GetNumBodies()));
		AE_PROFILE_PLOT("Phys.ActiveBodies", static_cast<int64_t>(m_impl->physics->GetNumActiveBodies(JPH::EBodyType::RigidBody)));
		m_impl->physics->Update(kFixedTimestep, /*collision_steps*/ 1, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
	}

	void PhysicsSystem::SyncTransforms(World& world, float alpha)
	{
		AE_PROFILE_ZONE_N("Phys.SyncTransforms");
		auto& bi = m_impl->physics->GetBodyInterfaceNoLock();

		int64_t synced = 0;
		for (const auto& [entity, rigid, state, transform]: world.View<RigidBodyComponent, PhysicsStateComponent, TransformComponent>().each())
		{
			(void) transform;
			const JPH::BodyID id = ToJolt(rigid.body);
			if (id.IsInvalid())
			{
				continue;
			}
			const Entity handle = World::FromEntt(entity);
			if (ecs::HasDisabledAncestor(world, handle))
			{
				if (bi.IsActive(id))
				{
					bi.DeactivateBody(id);
					m_impl->suspendedByDisable.insert(handle.id);
				}
				continue;
			}

			if (!bi.IsActive(id))
			{
				// Lifting the disable resumes exactly the bodies the branch above
				// deactivated; a body that fell asleep on its own stays asleep, and
				// statics never enter the set so they are never woken.
				if (m_impl->suspendedByDisable.erase(handle.id) > 0)
				{
					bi.ActivateBody(id);
				}
				continue;
			}

			JPH::RVec3 pos;
			JPH::Quat rot;
			bi.GetPositionAndRotation(id, pos, rot);
			state.currPosition = FromJolt(pos);
			state.currRotation = FromJolt(rot);

			const glm::vec3 renderPos = glm::mix(state.prevPosition, state.currPosition, alpha);
			const glm::quat renderRot = glm::slerp(state.prevRotation, state.currRotation, alpha);

			ecs::SetWorldTransform(world, handle, ToTransform(renderPos, renderRot, state.scale));
			++synced;
		}

		AE_PROFILE_PLOT("Phys.SyncedBodies", synced);
	}

	static glm::vec3 ExtractPosition(const TransformComponent& t)
	{
		return {t.localToWorld[3][0], t.localToWorld[3][1], t.localToWorld[3][2]};
	}

	static glm::quat ExtractRotation(const TransformComponent& t)
	{
		glm::mat3 rot(t.localToWorld);
		rot[0] = glm::normalize(rot[0]);
		rot[1] = glm::normalize(rot[1]);
		rot[2] = glm::normalize(rot[2]);
		return glm::quat_cast(rot);
	}

	static JPH::ShapeRefC GetOrCreateColliderShape(std::unordered_map<uint64_t, JPH::ShapeRefC>& cache, const ColliderComponent& c)
	{
		uint64_t key = 0;
		switch (c.shape)
		{
			case PhysicsShapeType::Box:
				key = BoxKey(c.halfExtents);
				break;
			case PhysicsShapeType::Sphere:
				key = SphereKey(c.radius);
				break;
			case PhysicsShapeType::Capsule:
				key = CapsuleKey(c.halfHeight, c.radius);
				break;
			case PhysicsShapeType::Cylinder:
				key = CylinderKey(c.halfHeight, c.radius);
				break;
		}
		if (const auto it = cache.find(key); it != cache.end())
		{
			return it->second;
		}

		JPH::ShapeSettings::ShapeResult result;
		switch (c.shape)
		{
			case PhysicsShapeType::Box:
				result = JPH::BoxShapeSettings{ToJolt(c.halfExtents)}.Create();
				break;
			case PhysicsShapeType::Sphere:
				result = JPH::SphereShapeSettings{c.radius}.Create();
				break;
			case PhysicsShapeType::Capsule:
				result = JPH::CapsuleShapeSettings{c.halfHeight, c.radius}.Create();
				break;
			case PhysicsShapeType::Cylinder:
				result = JPH::CylinderShapeSettings{c.halfHeight, c.radius}.Create();
				break;
		}
		if (result.HasError())
		{
			AE_WARN(LogCategory::Engine, "PhysicsSystem: collider shape error: {}", result.GetError().c_str());
			return {};
		}
		return cache.emplace(key, result.Get()).first->second;
	}

	static void ApplyRigidBodyTunables(JPH::BodyCreationSettings& bcs, const RigidBodyComponent& rb)
	{
		bcs.mLinearDamping = rb.linearDamping;
		bcs.mAngularDamping = rb.angularDamping;
		bcs.mGravityFactor = rb.gravityFactor;
		bcs.mMaxLinearVelocity = rb.maxLinearVelocity;
		bcs.mMaxAngularVelocity = rb.maxAngularVelocity;
		bcs.mAllowSleeping = rb.allowSleeping;
		bcs.mMotionQuality = rb.continuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;

		JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::None;
		if (!rb.lockPosition.x)
		{
			dofs = dofs | JPH::EAllowedDOFs::TranslationX;
		}
		if (!rb.lockPosition.y)
		{
			dofs = dofs | JPH::EAllowedDOFs::TranslationY;
		}
		if (!rb.lockPosition.z)
		{
			dofs = dofs | JPH::EAllowedDOFs::TranslationZ;
		}
		if (!rb.lockRotation.x)
		{
			dofs = dofs | JPH::EAllowedDOFs::RotationX;
		}
		if (!rb.lockRotation.y)
		{
			dofs = dofs | JPH::EAllowedDOFs::RotationY;
		}
		if (!rb.lockRotation.z)
		{
			dofs = dofs | JPH::EAllowedDOFs::RotationZ;
		}
		bcs.mAllowedDOFs = dofs;

		if (rb.mass > 0.0f)
		{
			bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			bcs.mMassPropertiesOverride.mMass = rb.mass;
		}
	}


	static JPH::Constraint* CreateJointConstraint(const JointComponent& j, JPH::Body& b1, JPH::Body& b2)
	{
		const JPH::RVec3 anchor(j.anchor.x, j.anchor.y, j.anchor.z);
		const glm::vec3 axisGlm = glm::length(j.axis) > 1e-6f ? glm::normalize(j.axis) : glm::vec3(0.0f, 1.0f, 0.0f);
		const JPH::Vec3 axis = ToJolt(axisGlm);

		switch (j.type)
		{
			case JointType::Fixed:
			{
				JPH::FixedConstraintSettings s;
				s.mAutoDetectPoint = true;
				return s.Create(b1, b2);
			}
			case JointType::Point:
			{
				JPH::PointConstraintSettings s;
				s.mSpace = JPH::EConstraintSpace::WorldSpace;
				s.mPoint1 = anchor;
				s.mPoint2 = anchor;
				return s.Create(b1, b2);
			}
			case JointType::Hinge:
			{
				JPH::HingeConstraintSettings s;
				s.mSpace = JPH::EConstraintSpace::WorldSpace;
				s.mPoint1 = anchor;
				s.mPoint2 = anchor;
				s.mHingeAxis1 = axis;
				s.mHingeAxis2 = axis;
				s.mNormalAxis1 = axis.GetNormalizedPerpendicular();
				s.mNormalAxis2 = axis.GetNormalizedPerpendicular();
				if (j.minLimit < j.maxLimit)
				{
					s.mLimitsMin = j.minLimit;
					s.mLimitsMax = j.maxLimit;
				}
				return s.Create(b1, b2);
			}
			case JointType::Distance:
			{
				JPH::DistanceConstraintSettings s;
				s.mSpace = JPH::EConstraintSpace::WorldSpace;
				s.mPoint1 = anchor;
				s.mPoint2 = anchor;
				if (j.distance >= 0.0f)
				{
					s.mMinDistance = j.distance;
					s.mMaxDistance = j.distance;
				}
				return s.Create(b1, b2);
			}
			case JointType::Slider:
			{
				JPH::SliderConstraintSettings s;
				s.mSpace = JPH::EConstraintSpace::WorldSpace;
				s.mPoint1 = anchor;
				s.mPoint2 = anchor;
				s.SetSliderAxis(axis);
				if (j.minLimit < j.maxLimit)
				{
					s.mLimitsMin = j.minLimit;
					s.mLimitsMax = j.maxLimit;
				}
				return s.Create(b1, b2);
			}
		}
		return nullptr;
	}

	void PhysicsSystem::FlushPendingBodies(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.FlushPending");
		auto& bi = m_impl->physics->GetBodyInterfaceNoLock();
		auto& reg = world.GetRegistry();
		bool addedStatic = false;

		AE_PROFILE_PLOT("Phys.Colliders", static_cast<int64_t>(world.View<ColliderComponent>().size()));

		for (const auto& [enttEntity, collider]: world.View<ColliderComponent>().each())
		{
			auto* rb = reg.try_get<RigidBodyComponent>(enttEntity);
			if (rb != nullptr && rb->body.IsValid())
			{
				continue;
			}
			// One entity never simulates in both physics domains.
			if (physics_gate::EntityHas2DPhysics(world, World::FromEntt(enttEntity)))
			{
				AE_WARN(LogCategory::Engine, "Entity {} has both 3D and 2D physics components; skipping its 3D body (remove one set)", World::FromEntt(enttEntity).id);
				continue;
			}

			JPH::ShapeRefC shape = GetOrCreateColliderShape(m_impl->shapeCache, collider);
			if (shape == nullptr)
			{
				continue;
			}
			if (glm::dot(collider.center, collider.center) > 1e-8f)
			{
				const JPH::RotatedTranslatedShapeSettings offsetSettings{ToJolt(collider.center), JPH::Quat::sIdentity(), shape};
				if (auto offsetResult = offsetSettings.Create(); !offsetResult.HasError())
				{
					shape = offsetResult.Get();
				}
			}

			const Entity entity{static_cast<uint32_t>(entt::to_integral(enttEntity))};
			auto* const tc = reg.try_get<TransformComponent>(enttEntity);
			const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
			const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);

			const PhysicsMotionType motion = rb != nullptr ? rb->motionType : PhysicsMotionType::Static;
			// Statics belong in the non-moving broad-phase tree: Jolt keeps that tree separate
			// precisely so it is not rebuilt every frame, and leaving them on Moving also pairs
			// every static against every other static in the broad phase for nothing.
			const PhysicsLayer layer = collider.isSensor        ? PhysicsLayer::Sensor
			        : motion == PhysicsMotionType::Static       ? PhysicsLayer::NonMoving
			                                                    : collider.layer;

			JPH::BodyCreationSettings bcs{shape, JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), ToJoltMotionType(motion), ToJoltLayer(layer)};
			bcs.mUserData = static_cast<JPH::uint64>(entity.id);
			bcs.mFriction = collider.friction;
			bcs.mRestitution = collider.restitution;
			bcs.mIsSensor = collider.isSensor;
			if (rb != nullptr)
			{
				ApplyRigidBodyTunables(bcs, *rb);
			}

			const JPH::Body* body = bi.CreateBody(bcs);
			if (body == nullptr)
			{
				AE_WARN(LogCategory::Engine, "PhysicsSystem: failed to create body - max body limit reached");
				continue;
			}
			const JPH::BodyID id = body->GetID();
			// added inactive, so it never simulates a step before SyncTransforms
			const bool disabled = ecs::HasDisabledAncestor(world, entity);
			const bool startActive = (rb != nullptr ? rb->startActive : true) && !disabled;
			bi.AddBody(id, startActive ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

			RigidBodyComponent& rbc = rb != nullptr ? *rb : world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Static});
			rbc.body = FromJolt(id);
			rbc.motionType = motion;

			// The authored entity scale is visual-only in 3D physics - collider
			// shapes are built from collider dimensions, never entity scale - so it
			// must survive body creation. Writing the collider's dimensions here
			// would silently resize the entity and persist through
			// PhysicsStateComponent::scale into every later SyncTransforms write.
			const glm::vec3 authoredScale = tc != nullptr ? ExtractScale(tc->localToWorld) : glm::vec3(1.0f);
			const glm::vec3 wpos = FromJolt(bi.GetPosition(id));
			const glm::quat wrot = FromJolt(bi.GetRotation(id));
			reg.emplace_or_replace<PhysicsStateComponent>(enttEntity, wpos, wrot, wpos, wrot, authoredScale);
			if (tc != nullptr)
			{
				tc->localToWorld = ToTransform(wpos, wrot, authoredScale);
			}

			if (motion == PhysicsMotionType::Static)
			{
				addedStatic = true;
			}
			if (rb != nullptr && glm::dot(rb->initialVelocity, rb->initialVelocity) > 0.f)
			{
				bi.SetLinearVelocity(id, ToJolt(rb->initialVelocity));
			}
		}

		if (addedStatic)
		{
			AE_PROFILE_ZONE_N("Phys.OptimizeBroadPhase");
			m_impl->physics->OptimizeBroadPhase();
		}
	}

	void PhysicsSystem::FlushPendingJoints(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.FlushJoints");
		auto& reg = world.GetRegistry();
		auto& physics = *m_impl->physics;
		const JPH::BodyLockInterface& bli = physics.GetBodyLockInterface();

		for (auto&& [enttE, joint]: reg.view<JointComponent>().each())
		{
			if (joint.constraintId != 0)
			{
				continue;
			}
			const auto* rbSelf = reg.try_get<RigidBodyComponent>(enttE);
			if (rbSelf == nullptr || !rbSelf->body.IsValid())
			{
				continue;
			}
			const JPH::BodyID selfId = ToJolt(rbSelf->body);

			JPH::BodyID otherId;
			if (joint.target.IsValid() && reg.valid(World::ToEntt(joint.target)))
			{
				const auto* rbOther = reg.try_get<RigidBodyComponent>(World::ToEntt(joint.target));
				if (rbOther == nullptr || !rbOther->body.IsValid())
				{
					continue;
				}
				otherId = ToJolt(rbOther->body);
			}

			auto joinGroup = [&](JPH::Body& body)
			{
				if (body.GetCollisionGroup().GetGroupFilter() != m_impl->groupFilter.GetPtr())
				{
					body.SetCollisionGroup(JPH::CollisionGroup(m_impl->groupFilter.GetPtr(), 0, body.GetID().GetIndex()));
				}
			};

			JPH::Constraint* created = nullptr;
			bool collisionDisabled = false;
			if (otherId.IsInvalid())
			{
				const JPH::BodyLockWrite lock(bli, selfId);
				if (lock.Succeeded())
				{
					created = CreateJointConstraint(joint, lock.GetBody(), JPH::Body::sFixedToWorld);
				}
			}
			else
			{
				const JPH::BodyID ids[2] = {selfId, otherId};
				const JPH::BodyLockMultiWrite locks(bli, ids, 2);
				JPH::Body* b1 = locks.GetBody(0);
				JPH::Body* b2 = locks.GetBody(1);
				if (b1 != nullptr && b2 != nullptr)
				{
					created = CreateJointConstraint(joint, *b1, *b2);
					if (created != nullptr && !joint.collideConnected)
					{
						joinGroup(*b1);
						joinGroup(*b2);
						m_impl->groupFilter->DisableCollision(b1->GetID().GetIndex(), b2->GetID().GetIndex());
						collisionDisabled = true;
					}
				}
			}
			if (created == nullptr)
			{
				continue;
			}
			created->SetEnabled(true);

			physics.AddConstraint(created);
			const std::uint32_t id = m_impl->nextConstraintId++;
			Impl::LiveConstraint live;
			live.constraint = created;
			live.bodyA = selfId.GetIndex();
			live.bodyB = otherId.IsInvalid() ? 0u : otherId.GetIndex();
			live.collisionDisabled = collisionDisabled;
			m_impl->constraints.emplace(id, std::move(live));
			joint.constraintId = id;
		}
	}

	void PhysicsSystem::DrainContactEvents(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.DrainContacts");
		if (m_impl->contactCollector == nullptr)
		{
			return;
		}

		auto& reg = world.GetRegistry();
		for (auto&& [enttE, ev]: reg.view<CollisionEventsComponent>().each())
		{
			ev.collisionEnter.clear();
			ev.collisionExit.clear();
			ev.triggerEnter.clear();
			ev.triggerExit.clear();
		}

		auto& added = m_impl->addedScratch;
		auto& removed = m_impl->removedScratch;
		m_impl->contactCollector->Take(added, removed);
		if (added.empty() && removed.empty())
		{
			return;
		}

		auto alive = [&](Entity e)
		{
			return e.IsValid() && reg.valid(World::ToEntt(e));
		};
		auto record = [&](Entity self, Entity other, bool sensor, bool entered)
		{
			auto* ev = reg.try_get<CollisionEventsComponent>(World::ToEntt(self));
			if (ev == nullptr)
			{
				return;
			}
			if (entered)
			{
				(sensor ? ev->triggerEnter : ev->collisionEnter).push_back(other);
				if (std::find(ev->overlapping.begin(), ev->overlapping.end(), other) == ev->overlapping.end())
				{
					ev->overlapping.push_back(other);
				}
			}
			else
			{
				(sensor ? ev->triggerExit : ev->collisionExit).push_back(other);
				std::erase(ev->overlapping, other);
			}
		};

		for (const auto& e: added)
		{
			const Entity a{static_cast<std::uint32_t>(e.entity1)};
			const Entity b{static_cast<std::uint32_t>(e.entity2)};
			if (alive(a) && alive(b))
			{
				record(a, b, e.sensor, true);
				record(b, a, e.sensor, true);
			}
		}

		auto& bi = m_impl->physics->GetBodyInterfaceNoLock();
		for (const auto& r: removed)
		{
			if (!bi.IsAdded(r.body1) || !bi.IsAdded(r.body2))
			{
				continue;
			}
			const Entity a{static_cast<std::uint32_t>(bi.GetUserData(r.body1))};
			const Entity b{static_cast<std::uint32_t>(bi.GetUserData(r.body2))};
			if (!alive(a) || !alive(b))
			{
				continue;
			}
			const auto* ca = world.TryGet<ColliderComponent>(a);
			const auto* cb = world.TryGet<ColliderComponent>(b);
			const bool sensor = (ca != nullptr && ca->isSensor) || (cb != nullptr && cb->isSensor);
			record(a, b, sensor, false);
		}
	}

	void PhysicsSystem::RemoveJointConstraint(std::uint32_t constraintId)
	{
		const auto it = m_impl->constraints.find(constraintId);
		if (it == m_impl->constraints.end())
		{
			return;
		}
		m_impl->physics->RemoveConstraint(it->second.constraint.GetPtr());
		if (it->second.collisionDisabled && m_impl->groupFilter != nullptr)
		{
			m_impl->groupFilter->EnableCollision(it->second.bodyA, it->second.bodyB);
		}
		m_impl->constraints.erase(it);
	}

	void PhysicsSystem::RebuildJoint(World& world, Entity entity)
	{
		auto* joint = world.TryGet<JointComponent>(entity);
		if (joint == nullptr)
		{
			return;
		}
		WaitForStep();
		if (joint->constraintId != 0)
		{
			RemoveJointConstraint(joint->constraintId);
			joint->constraintId = 0;
		}
	}

	void PhysicsSystem::OnJointDestroyed(entt::registry& registry, entt::entity enttEntity)
	{
		WaitForStep();
		if (!m_impl->physics)
		{
			return;
		}
		auto* joint = registry.try_get<JointComponent>(enttEntity);
		if (joint == nullptr || joint->constraintId == 0)
		{
			return;
		}
		RemoveJointConstraint(joint->constraintId);
		joint->constraintId = 0;
	}

	void PhysicsSystem::RemoveBody(World& world, Entity entity)
	{
		WaitForStep();

		auto* rigid = world.TryGet<RigidBodyComponent>(entity);
		if (!rigid || !rigid->body.IsValid())
		{
			return;
		}

		// Any live joint referencing this body would keep a JPH::Constraint pointed
		// at the body destroyed below (and its collision-disable pair registered).
		// OnRigidBodyDestroyed does this same cleanup on the on_destroy signal, but
		// this path clears rigid->body first, so the signal handler bails out early.
		DestroyJointsTouching(world.GetRegistry(), World::ToEntt(entity));
		m_impl->suspendedByDisable.erase(entity.id);

		auto& bodyInterface = m_impl->physics->GetBodyInterfaceNoLock();
		const JPH::BodyID id = ToJolt(rigid->body);

		rigid->body = {};

		bodyInterface.RemoveBody(id);
		bodyInterface.DestroyBody(id);

		world.Remove<RigidBodyComponent>(entity);
		world.Remove<PhysicsStateComponent>(entity);
		world.Remove<ColliderComponent>(entity);
	}

	void PhysicsSystem::DestroyJointsTouching(entt::registry& registry, entt::entity enttEntity)
	{
		if (m_impl->constraints.empty())
		{
			return;
		}
		for (auto&& [je, joint]: registry.view<JointComponent>().each())
		{
			if (joint.constraintId == 0)
			{
				continue;
			}
			const bool touches = je == enttEntity || (joint.target.IsValid() && World::ToEntt(joint.target) == enttEntity);
			if (!touches)
			{
				continue;
			}
			RemoveJointConstraint(joint.constraintId);
			joint.constraintId = 0;
		}
	}

	void PhysicsSystem::OnRigidBodyDestroyed(entt::registry& registry, entt::entity enttEntity)
	{
		WaitForStep();
		// The suspend-tracking entry must not outlive the body it names: a
		// recreated body would be wrongly activated on re-enable.
		m_impl->suspendedByDisable.erase(World::FromEntt(enttEntity).id);

		if (!m_impl->physics)
		{
			return;
		}

		auto* rigid = registry.try_get<RigidBodyComponent>(enttEntity);
		if (!rigid || !rigid->body.IsValid())
		{
			return;
		}

		DestroyJointsTouching(registry, enttEntity);

		auto& bodyInterface = m_impl->physics->GetBodyInterfaceNoLock();
		const JPH::BodyID id = ToJolt(rigid->body);
		bodyInterface.RemoveBody(id);
		bodyInterface.DestroyBody(id);
	}

	void PhysicsSystem::SetLinearVelocity(PhysicsBodyHandle body, glm::vec3 v)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().SetLinearVelocity(id, ToJolt(v));
	}

	glm::vec3 PhysicsSystem::GetLinearVelocity(PhysicsBodyHandle body)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return {};
		}
		return FromJolt(m_impl->physics->GetBodyInterfaceNoLock().GetLinearVelocity(id));
	}

	void PhysicsSystem::SetAngularVelocity(PhysicsBodyHandle body, glm::vec3 v)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().SetAngularVelocity(id, ToJolt(v));
	}

	glm::vec3 PhysicsSystem::GetAngularVelocity(PhysicsBodyHandle body)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return {};
		}
		return FromJolt(m_impl->physics->GetBodyInterfaceNoLock().GetAngularVelocity(id));
	}

	void PhysicsSystem::AddImpulse(PhysicsBodyHandle body, glm::vec3 impulse)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().AddImpulse(id, ToJolt(impulse));
	}

	void PhysicsSystem::AddForce(PhysicsBodyHandle body, glm::vec3 force)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().AddForce(id, ToJolt(force));
	}

	void PhysicsSystem::AddTorque(PhysicsBodyHandle body, glm::vec3 torque)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().AddTorque(id, ToJolt(torque));
	}

	void PhysicsSystem::AddAngularImpulse(PhysicsBodyHandle body, glm::vec3 angularImpulse)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().AddAngularImpulse(id, ToJolt(angularImpulse));
	}

	void PhysicsSystem::SetPosition(PhysicsBodyHandle body, glm::vec3 pos)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().SetPosition(id, JPH::RVec3(pos.x, pos.y, pos.z), JPH::EActivation::Activate);
	}

	void PhysicsSystem::SetRotation(PhysicsBodyHandle body, glm::quat rot)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().SetRotation(id, ToJolt(rot), JPH::EActivation::Activate);
	}

	void PhysicsSystem::SetFriction(PhysicsBodyHandle body, float friction)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().SetFriction(id, friction);
	}

	void PhysicsSystem::SetRestitution(PhysicsBodyHandle body, float restitution)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().SetRestitution(id, restitution);
	}

	void PhysicsSystem::SetGravityFactor(PhysicsBodyHandle body, float factor)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().SetGravityFactor(id, factor);
	}

	void PhysicsSystem::SetBodyActive(PhysicsBodyHandle body, bool active)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		auto& bi = m_impl->physics->GetBodyInterfaceNoLock();
		if (active)
		{
			bi.ActivateBody(id);
		}
		else
		{
			bi.DeactivateBody(id);
		}
	}

	bool PhysicsSystem::IsBodyActive(PhysicsBodyHandle body)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return false;
		}
		return m_impl->physics->GetBodyInterfaceNoLock().IsActive(id);
	}

	void PhysicsSystem::RebuildBody(World& world, Entity entity)
	{
		auto* rb = world.TryGet<RigidBodyComponent>(entity);
		if (rb == nullptr || !world.Has<ColliderComponent>(entity))
		{
			return;
		}

		WaitForStep();
		if (rb->body.IsValid())
		{
			auto& reg = world.GetRegistry();
			const entt::entity enttEntity = World::ToEntt(entity);
			DestroyJointsTouching(reg, enttEntity);

			auto& bi = m_impl->physics->GetBodyInterfaceNoLock();
			const JPH::BodyID id = ToJolt(rb->body);
			rb->initialVelocity = FromJolt(bi.GetLinearVelocity(id));
			rb->body = {};
			m_impl->suspendedByDisable.erase(entity.id);
			bi.RemoveBody(id);
			bi.DestroyBody(id);
		}
		world.Remove<PhysicsStateComponent>(entity);
	}

	void PhysicsSystem::WaitForStepIdle()
	{
		WaitForStep();
	}

	void PhysicsSystem::FlushPendingOnly(World& world)
	{
		AE_PROFILE_ZONE();
		// from the frame Play toggled off must still be waited out once.
		WaitForStep();
		FlushPendingBodies(world);
		FlushPendingJoints(world);
	}

	PhysicsSystem::RaycastResult PhysicsSystem::CastRay(glm::vec3 origin, glm::vec3 direction, float maxDistance)
	{
		WaitForStep();

		RaycastResult result{};

		if (maxDistance <= 0.0f || glm::length(direction) < 0.001f)
		{
			return result;
		}

		direction = glm::normalize(direction);

		const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), maxDistance * JPH::Vec3(direction.x, direction.y, direction.z));

		JPH::RayCastResult joltResult;

		m_impl->physics->GetNarrowPhaseQuery().CastRay(ray, joltResult, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{}, JPH::BodyFilter{});

		if (!joltResult.mBodyID.IsInvalid())
		{
			result.hit = true;
			result.body = FromJolt(joltResult.mBodyID);
			result.entity = static_cast<std::uint32_t>(m_impl->physics->GetBodyInterfaceNoLock().GetUserData(joltResult.mBodyID));
			result.fraction = joltResult.mFraction;

			const JPH::RVec3 hitPosR = ray.GetPointOnRay(joltResult.mFraction);
			result.position = {hitPosR.GetX(), hitPosR.GetY(), hitPosR.GetZ()};

			// Surface normal: lock the body and query the shape.
			const JPH::BodyLockRead lock(m_impl->physics->GetBodyLockInterface(), joltResult.mBodyID);
			if (lock.Succeeded())
			{
				const JPH::Body& body = lock.GetBody();
				const JPH::Vec3 surfaceNormal = body.GetWorldSpaceSurfaceNormal(joltResult.mSubShapeID2, ray.GetPointOnRay(joltResult.mFraction));
				result.normal = FromJolt(surfaceNormal);
			}
			else
			{
				result.normal = {0.0f, 1.0f, 0.0f};
			}
		}

		return result;
	}

	PhysicsSystem::RaycastResult PhysicsSystem::SphereCast(glm::vec3 origin, glm::vec3 direction, float radius, float maxDistance)
	{
		WaitForStep();
		RaycastResult result{};
		if (maxDistance <= 0.0f || radius <= 0.0f || glm::length(direction) < 0.001f)
		{
			return result;
		}
		direction = glm::normalize(direction);

		JPH::SphereShape sphere(radius);
		sphere.SetEmbedded();

		const JPH::RShapeCast cast(&sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(JPH::RVec3(origin.x, origin.y, origin.z)), maxDistance * JPH::Vec3(direction.x, direction.y, direction.z));
		const JPH::ShapeCastSettings settings;
		JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
		m_impl->physics->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector);

		if (collector.HadHit())
		{
			result.hit = true;
			result.body = FromJolt(collector.mHit.mBodyID2);
			result.entity = static_cast<std::uint32_t>(m_impl->physics->GetBodyInterfaceNoLock().GetUserData(collector.mHit.mBodyID2));
			result.fraction = collector.mHit.mFraction;
			result.position = FromJolt(collector.mHit.mContactPointOn2);
			result.normal = FromJolt(-collector.mHit.mPenetrationAxis.Normalized());
		}
		return result;
	}

	std::vector<std::uint32_t> PhysicsSystem::OverlapSphere(glm::vec3 center, float radius)
	{
		WaitForStep();
		std::vector<std::uint32_t> out;
		if (radius <= 0.0f)
		{
			return out;
		}

		JPH::SphereShape sphere(radius);
		sphere.SetEmbedded();

		const JPH::RMat44 com = JPH::RMat44::sTranslation(JPH::RVec3(center.x, center.y, center.z));
		const JPH::CollideShapeSettings settings;
		JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
		m_impl->physics->GetNarrowPhaseQuery().CollideShape(&sphere, JPH::Vec3::sReplicate(1.0f), com, settings, JPH::RVec3::sZero(), collector);

		auto& bi = m_impl->physics->GetBodyInterfaceNoLock();
		out.reserve(collector.mHits.size());
		for (const JPH::CollideShapeResult& hit: collector.mHits)
		{
			if (bi.IsAdded(hit.mBodyID2))
			{
				out.push_back(static_cast<std::uint32_t>(bi.GetUserData(hit.mBodyID2)));
			}
		}
		return out;
	}

} // namespace aether
