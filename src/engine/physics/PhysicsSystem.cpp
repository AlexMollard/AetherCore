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
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
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
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
#include "assets/GltfAsset.hpp"
#include "physics/ColliderMeshSource.hpp"
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

	// ConvexHull and Mesh both key off the SAME path but must never alias each other -
	// a hull and the exact triangle mesh of the same source file are different shapes.
	// A shared cache means a scene with fifty copies of the same prop only loads and
	// hulls/triangulates its source mesh once.
	static uint64_t MeshSourceKey(std::string_view path, uint64_t discriminant)
	{
		const uint64_t hashed = std::hash<std::string_view>{}(path);
		return (hashed & ((uint64_t(1) << 61) - 1)) | (discriminant << 61);
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

		// Which entity's ScriptJointsComponent a given handle's JointEntry lives on -
		// CreateFixedConstraint/CreateDistanceConstraint populate this the moment a
		// handle is minted (before the entry is even flushed to Jolt), so
		// DestroyConstraint can find it by handle alone without a registry-wide scan.
		std::unordered_map<std::uint32_t, Entity> scriptJointOwners;

		// Bodies pulled out of the simulation because their hierarchy was disabled,
		// keyed by entity id; SyncTransforms re-activates exactly these on re-enable.
		std::unordered_set<std::uint32_t> suspendedByDisable;

		std::vector<ContactCollector::Added> addedScratch;
		std::vector<ContactCollector::Removed> removedScratch;

		// One JPH::CharacterVirtual per CharacterControllerComponent entity, keyed by
		// Entity::id. A CharacterVirtual is never added to the physics system's own body
		// list (see the class comment on CharacterVirtual), so unlike rigid bodies it has
		// no PhysicsBodyHandle - this map IS its storage.
		//
		// `io` is a full copy of the ECS component: the ONLY channel between
		// FlushPendingCharacters/SyncTransforms (game thread) and StepCharacters (physics
		// thread) - see StepCharacters' declaration for why it never touches entt
		// directly. FlushPendingCharacters refreshes io's authored/input fields from the
		// live ECS component once per Update(); StepCharacters reads/writes it freely
		// while the physics thread exclusively owns it; SyncTransforms pulls its output
		// fields back out once the physics thread is idle again.
		//
		// builtRadius/builtHalfHeight are the dimensions the live Jolt shape was actually
		// BUILT with (as opposed to io.radius/io.halfHeight, which may already hold a
		// newer authored value), so StepCharacters can detect an inspector/MCP edit and
		// rebuild the shape without diffing every field every step.
		struct LiveCharacter
		{
			JPH::Ref<JPH::CharacterVirtual> character;
			float builtRadius = 0.0f;
			float builtHalfHeight = 0.0f;
			CharacterControllerComponent io;
		};

		std::unordered_map<std::uint32_t, LiveCharacter> characters;
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
		m_scriptJointsDestroyConn = world.GetRegistry().on_destroy<ScriptJointsComponent>().connect<&PhysicsSystem::OnScriptJointsDestroyed>(this);
		m_characterDestroyConn = world.GetRegistry().on_destroy<CharacterControllerComponent>().connect<&PhysicsSystem::OnCharacterControllerDestroyed>(this);

		AE_INFO(LogCategory::Engine, "PhysicsSystem initialised (Jolt, {} worker threads, fixed dt = {:.4f} s, dedicated physics thread)", workerThreads, kFixedTimestep);

		StartPhysicsThread();
	}

	void PhysicsSystem::OnUnregister([[maybe_unused]] World& world)
	{
		AE_PROFILE_ZONE();
		StopPhysicsThread();

		m_impl->constraints.clear();
		m_impl->characters.clear();
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
		FlushPendingScriptJoints(world);
		FlushPendingCharacters(world);
		PushKinematicTargets(world);

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
		// Before the Jolt body step, in lockstep with it - see the class comment on
		// Impl::LiveCharacter for why this touches no ECS state, and CharacterVirtual.h's
		// own class comment for why a virtual character must be driven by hand rather than
		// tracked automatically by JPH::PhysicsSystem::Update below.
		StepCharacters(kFixedTimestep);
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

			// A Kinematic body is transform-driven (scripts, or network replication
			// via NetworkContext::SyncSimulationAuthority forcing a non-owned body
			// Kinematic) - PushKinematicTargets pushes its ECS pose into Jolt, and
			// reading it back here would just overwrite that authored pose with
			// itself one frame late. A Static body never moves either. Only a
			// Dynamic body's position is actually decided by Jolt's own integration.
			if (rigid.motionType != PhysicsMotionType::Dynamic)
			{
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

		// Character controllers: same interpolation, but the physics-thread state lives in
		// Impl::LiveCharacter (see StepCharacters) rather than a Jolt Body, so this pulls
		// position/rotation/output straight from there instead of a BodyInterface. Safe to
		// read live.character here (this runs right after WaitForStep() at the top of
		// Update(), so the physics thread is guaranteed idle - the same guarantee the rigid
		// body loop above relies on).
		int64_t syncedCharacters = 0;
		for (const auto& [entity, cc, state, transform]: world.View<CharacterControllerComponent, PhysicsStateComponent, TransformComponent>().each())
		{
			(void) transform;
			const Entity handle = World::FromEntt(entity);
			const auto it = m_impl->characters.find(handle.id);
			if (it == m_impl->characters.end())
			{
				continue; // not yet baked by FlushPendingCharacters
			}
			const Impl::LiveCharacter& live = it->second;
			cc.isGrounded = live.io.isGrounded;
			cc.groundNormal = live.io.groundNormal;
			cc.velocity = live.io.velocity;

			if (ecs::HasDisabledAncestor(world, handle))
			{
				continue;
			}

			state.currPosition = FromJolt(live.character->GetPosition());
			state.currRotation = FromJolt(live.character->GetRotation());

			const glm::vec3 renderPos = glm::mix(state.prevPosition, state.currPosition, alpha);
			const glm::quat renderRot = glm::slerp(state.prevRotation, state.currRotation, alpha);

			ecs::SetWorldTransform(world, handle, ToTransform(renderPos, renderRot, state.scale));
			++syncedCharacters;
		}
		AE_PROFILE_PLOT("Phys.SyncedCharacters", syncedCharacters);
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

	void PhysicsSystem::PushKinematicTargets(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.PushKinematicTargets");
		auto& bi = m_impl->physics->GetBodyInterfaceNoLock();
		for (const auto& [enttEntity, rigid, state, transform]: world.View<RigidBodyComponent, PhysicsStateComponent, TransformComponent>().each())
		{
			if (rigid.motionType != PhysicsMotionType::Kinematic)
			{
				continue;
			}
			const JPH::BodyID id = ToJolt(rigid.body);
			if (id.IsInvalid())
			{
				continue;
			}
			const glm::vec3 pos = ExtractPosition(transform);
			const glm::quat rot = ExtractRotation(transform);
			// MoveKinematic, not a teleport: it sets the body's velocity such that Jolt's
			// OWN solver carries it from its current pose to (pos, rot) over kFixedTimestep,
			// so a network-driven crate sliding through a stack of props actually shoves
			// them - a straight SetPositionAndRotation places the body silently and imparts
			// no motion to anything it touches, which is indistinguishable from every other
			// body simply teleporting through it. This is the authoritative pose every
			// Kinematic body here follows (SyncSimulationAuthority forces non-owned bodies
			// Kinematic precisely so a peer's own transform replication drives them this
			// way), so it deserves the same real-motion treatment an owned Dynamic body gets.
			bi.MoveKinematic(id, JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), kFixedTimestep);
			state.prevPosition = state.currPosition = pos;
			state.prevRotation = state.currRotation = rot;
		}
	}

	struct MeshSourceGeometry
	{
		JPH::Array<JPH::Vec3> positions;       // hull input
		JPH::VertexList triangleVertices;      // same positions as Float3 - MeshShapeSettings input
		JPH::IndexedTriangleList triangles;    // indices offset per-primitive, winding as authored
	};

	// Loads a mesh asset's raw CPU-side vertex positions (and, for a triangle-mesh
	// shape, its indices) through the SAME glTF/.mesh loader RagdollBuilder already
	// uses for skeleton data (assets::GltfAsset::LoadFromVfsPath) - this is CPU-only
	// baked-asset data and never touches the GPU-resident Mesh/AssetManager path
	// (engine::Mesh only stores GPU buffer handles once uploaded; there is no CPU copy
	// left to read back from there). Runs once per UNIQUE meshSource path per process:
	// GetOrCreateColliderShape's own shape cache means a scene with fifty copies of the
	// same prop only pays this cost once, and it happens at body-creation time (a
	// one-shot event per spawned entity), not once per frame. The actual vertex/index
	// gathering and vertex-count cap live in ColliderMeshSource.cpp, pure and free of
	// Jolt types, so they are unit-testable against a hand-built GltfAsset without
	// loading a real file - this function only adds the file load and Jolt-type
	// conversion around that.
	std::optional<MeshSourceGeometry> LoadMeshSourceGeometry(std::string_view path)
	{
		const auto asset = assets::GltfAsset::LoadFromVfsPath(path);
		if (!asset.has_value())
		{
			AE_WARN(LogCategory::Engine, "PhysicsSystem: collider mesh_source '{}' failed to load: {}", path, asset.error());
			return std::nullopt;
		}
		const std::optional<ColliderMeshGeometry> geo = BuildColliderMeshGeometry(*asset);
		if (!geo.has_value())
		{
			AE_WARN(LogCategory::Engine, "PhysicsSystem: collider mesh_source '{}' has no vertices, or exceeds the {}-vertex collider limit - use a simpler mesh or a primitive shape", path, kMaxColliderMeshVertices);
			return std::nullopt;
		}

		MeshSourceGeometry out;
		out.positions.reserve(geo->positions.size());
		out.triangleVertices.reserve(geo->positions.size());
		for (const glm::vec3& p: geo->positions)
		{
			out.positions.emplace_back(p.x, p.y, p.z);
			out.triangleVertices.emplace_back(p.x, p.y, p.z);
		}
		out.triangles.reserve(geo->indices.size() / 3);
		for (std::size_t i = 0; i + 2 < geo->indices.size(); i += 3)
		{
			out.triangles.emplace_back(geo->indices[i], geo->indices[i + 1], geo->indices[i + 2], 0);
		}
		return out;
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
			case PhysicsShapeType::ConvexHull:
				key = MeshSourceKey(c.meshSource, 4);
				break;
			case PhysicsShapeType::Mesh:
				key = MeshSourceKey(c.meshSource, 5);
				break;
		}
		if (const auto it = cache.find(key); it != cache.end())
		{
			// A cached nullptr is a REMEMBERED FAILURE, not "not cached yet" - see
			// every failure path below. Returning it here (instead of falling through
			// to retry) is the whole fix: FlushPendingBodies runs every physics tick
			// for any entity that never got a valid body, so without this a broken
			// mesh_source re-attempted its file load and re-logged its warning 60
			// times a second, forever, for as long as the entity existed - drowning
			// out every other log line, including the render-mesh load failure for
			// the SAME broken asset (see PropSpawner.cs's own LoadModel call).
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
			case PhysicsShapeType::ConvexHull:
			{
				const std::optional<MeshSourceGeometry> geo = LoadMeshSourceGeometry(c.meshSource);
				if (!geo.has_value())
				{
					cache.emplace(key, nullptr);
					return nullptr;
				}
				result = JPH::ConvexHullShapeSettings{geo->positions}.Create();
				break;
			}
			case PhysicsShapeType::Mesh:
			{
				const std::optional<MeshSourceGeometry> geo = LoadMeshSourceGeometry(c.meshSource);
				if (!geo.has_value())
				{
					cache.emplace(key, nullptr);
					return nullptr;
				}
				if (geo->triangles.empty())
				{
					AE_WARN(LogCategory::Engine, "PhysicsSystem: collider mesh_source '{}' produced no triangles (unindexed or fully-degenerate mesh)", c.meshSource);
					cache.emplace(key, nullptr);
					return nullptr;
				}
				result = JPH::MeshShapeSettings{geo->triangleVertices, geo->triangles}.Create();
				break;
			}
		}
		if (result.HasError())
		{
			AE_WARN(LogCategory::Engine, "PhysicsSystem: collider shape error: {}", result.GetError().c_str());
			cache.emplace(key, nullptr);
			return nullptr;
		}
		return cache.emplace(key, result.Get()).first->second;
	}

	// Character capsules are not cached like collider shapes: they change per-entity via
	// live inspector edits (StepCharacters rebuilds on a radius/halfHeight change) far more
	// often than collider dimensions do, so the cache would mostly hold one-shot entries.
	static JPH::ShapeRefC MakeCharacterCapsuleShape(float halfHeight, float radius)
	{
		JPH::CapsuleShapeSettings capsuleSettings{halfHeight, radius};
		const JPH::ShapeSettings::ShapeResult capsuleResult = capsuleSettings.Create();
		if (capsuleResult.HasError())
		{
			AE_WARN(LogCategory::Engine, "PhysicsSystem: character capsule shape error: {}", capsuleResult.GetError().c_str());
			return {};
		}
		// CharacterVirtual expects the shape's bottom at local (0,0,0) - its mPosition
		// tracks the character's FEET, not its centre - but CapsuleShape is centred on its
		// own middle, so lift it by half-height + radius, exactly like Jolt's own character
		// samples (CharacterBaseTest::sCreateCapsuleShape).
		const JPH::RotatedTranslatedShapeSettings offsetSettings{JPH::Vec3(0.0f, halfHeight + radius, 0.0f), JPH::Quat::sIdentity(), capsuleResult.Get()};
		const JPH::ShapeSettings::ShapeResult offsetResult = offsetSettings.Create();
		if (offsetResult.HasError())
		{
			AE_WARN(LogCategory::Engine, "PhysicsSystem: character capsule offset error: {}", offsetResult.GetError().c_str());
			return {};
		}
		return offsetResult.Get();
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


	// Templated so both JointComponent (one authored joint per entity) and JointEntry
	// (many script-created joints per entity - see JointEntry's own comment) drive the
	// same Jolt constraint construction from the same field shape without duplicating
	// this switch. Requires only that JointLike exposes type/anchor/axis/minLimit/
	// maxLimit/distance/swingLimit, which both do.
	template<typename JointLike>
	static JPH::Constraint* CreateJointConstraint(const JointLike& j, JPH::Body& b1, JPH::Body& b2)
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
			case JointType::SwingTwist:
			{
				JPH::SwingTwistConstraintSettings s;
				s.mSpace = JPH::EConstraintSpace::WorldSpace;
				s.mPosition1 = anchor;
				s.mPosition2 = anchor;
				s.mTwistAxis1 = axis;
				s.mTwistAxis2 = axis;
				s.mPlaneAxis1 = axis.GetNormalizedPerpendicular();
				s.mPlaneAxis2 = axis.GetNormalizedPerpendicular();
				s.mNormalHalfConeAngle = j.swingLimit;
				s.mPlaneHalfConeAngle = j.swingLimit;
				// Jolt's own default twist range is [0, 0] (fully locked) - unlike Hinge's
				// own default of [-pi, pi], so "unspecified" has to be filled in by hand here
				// to keep the same "equal limits means free" convention every other joint uses.
				s.mTwistMinAngle = j.minLimit < j.maxLimit ? j.minLimit : -glm::pi<float>();
				s.mTwistMaxAngle = j.minLimit < j.maxLimit ? j.maxLimit : glm::pi<float>();
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
			// A character controller shape-owns its entity the same way a rigid body does
			// (see CharacterControllerComponent) - the two must never both try to sync
			// PhysicsStateComponent for the same entity.
			if (reg.any_of<CharacterControllerComponent>(enttEntity))
			{
				AE_WARN(LogCategory::Engine, "Entity {} has both a Rigid Body/Collider and a Character Controller; skipping its rigid body (remove one set)", World::FromEntt(enttEntity).id);
				continue;
			}

			const PhysicsMotionType motion = rb != nullptr ? rb->motionType : PhysicsMotionType::Static;
			// Jolt's own MeshShape::MustBeStatic() is advisory only - nothing in Jolt's
			// body-creation path enforces it, so this engine must reject the mismatch
			// itself rather than hand Jolt a triangle-mesh shape on a moving body (per
			// MeshShape's own doc comment, undefined mass/behaviour would follow).
			// ConvexHull has no such restriction (Jolt reduces it to a genuine convex
			// surface, usable on any motion type) - only the exact-triangle Mesh shape
			// needs this guard.
			if (collider.shape == PhysicsShapeType::Mesh && motion != PhysicsMotionType::Static)
			{
				AE_WARN(LogCategory::Engine, "Entity {} has a Mesh collider on a non-Static body - Jolt requires MeshShape bodies to be Static. Use ConvexHull for a moving mesh-sourced collider, or set motion to Static. Skipping its body.", World::FromEntt(enttEntity).id);
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
			if (rb != nullptr && glm::dot(rb->initialAngularVelocity, rb->initialAngularVelocity) > 0.f)
			{
				bi.SetAngularVelocity(id, ToJolt(rb->initialAngularVelocity));
			}
		}

		if (addedStatic)
		{
			AE_PROFILE_ZONE_N("Phys.OptimizeBroadPhase");
			m_impl->physics->OptimizeBroadPhase();
		}
	}

	void PhysicsSystem::FlushPendingCharacters(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.FlushPendingCharacters");
		auto& reg = world.GetRegistry();
		AE_PROFILE_PLOT("Phys.CharacterControllers", static_cast<int64_t>(world.View<CharacterControllerComponent>().size()));

		for (auto&& [enttEntity, cc]: world.View<CharacterControllerComponent>().each())
		{
			const Entity entity = World::FromEntt(enttEntity);
			const auto existing = m_impl->characters.find(entity.id);
			if (existing != m_impl->characters.end())
			{
				// Refresh the physics-thread-owned snapshot with this frame's authored
				// tunables and script input - see Impl::LiveCharacter. The one-shot jump
				// request is consumed here (not inside StepCharacters), so however many
				// fixed substeps run this frame, the jump is armed for exactly one of them.
				Impl::LiveCharacter& live = existing->second;
				CharacterControllerComponent& io = live.io;
				io.radius = cc.radius;
				io.halfHeight = cc.halfHeight;
				io.maxSlopeAngle = cc.maxSlopeAngle;
				io.stepHeight = cc.stepHeight;
				io.groundSnapDistance = cc.groundSnapDistance;
				io.mass = cc.mass;
				io.maxPushForce = cc.maxPushForce;
				io.gravityScale = cc.gravityScale;
				io.locallySimulated = cc.locallySimulated;
				io.desiredVelocity = cc.desiredVelocity;
				io.velocityOverride = cc.velocityOverride;
				io.pendingJumpSpeed = cc.pendingJumpSpeed;
				cc.pendingJumpSpeed = 0.0f;

				if (!cc.locallySimulated)
				{
					// Non-owner shadow (see CharacterControllerComponent::locallySimulated):
					// mirror whatever already wrote TransformComponent (replication) into the
					// Jolt-side character now, on the game thread, while the physics thread is
					// idle. StepCharacters sees locallySimulated == false and leaves it here
					// rather than integrating input/gravity against it.
					if (const auto* tc = reg.try_get<TransformComponent>(enttEntity))
					{
						live.character->SetPosition(ToJolt(ExtractPosition(*tc)));
						live.character->SetRotation(ToJolt(ExtractRotation(*tc)));
					}
				}
				continue;
			}

			if (reg.any_of<RigidBodyComponent, ColliderComponent>(enttEntity))
			{
				AE_WARN(LogCategory::Engine, "Entity {} has both a Character Controller and a Rigid Body/Collider; skipping its character body (remove one set)", entity.id);
				continue;
			}

			const JPH::ShapeRefC shape = MakeCharacterCapsuleShape(cc.halfHeight, cc.radius);
			if (shape == nullptr)
			{
				continue;
			}

			auto* const tc = reg.try_get<TransformComponent>(enttEntity);
			const glm::vec3 pos = tc != nullptr ? ExtractPosition(*tc) : glm::vec3(0.f);
			const glm::quat rot = tc != nullptr ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);

			JPH::CharacterVirtualSettings settings;
			settings.mShape = shape;
			settings.mUp = JPH::Vec3::sAxisY();
			settings.mMaxSlopeAngle = cc.maxSlopeAngle;
			settings.mMass = cc.mass;
			settings.mMaxStrength = cc.maxPushForce;
			// Inner rigid body: gives the character presence in the world outside its own
			// collision queries - see the class comment on CharacterControllerComponent for
			// what this costs and CharacterVirtual's own class comment for why a virtual
			// character needs one at all (it is never added to the broad phase itself).
			// Kinematic, created and destroyed automatically by CharacterVirtual's own
			// constructor/destructor, and kept in sync automatically too: Jolt calls
			// UpdateInnerBodyTransform() from inside SetPosition/SetRotation and at the end
			// of every Update()/ExtendedUpdate() call, so nothing here has to remember to
			// push a transform - only StepCharacters' shape-rebuild path has to remember to
			// call SetInnerBodyShape after SetShape (Jolt does not do that half itself).
			settings.mInnerBodyShape = shape;
			settings.mInnerBodyLayer = Layers::kMoving;

			Impl::LiveCharacter live;
			live.character = new JPH::CharacterVirtual(&settings, JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), static_cast<JPH::uint64>(entity.id), m_impl->physics.get());
			live.builtRadius = cc.radius;
			live.builtHalfHeight = cc.halfHeight;
			live.io = cc;
			// A jump requested before the body even existed has nothing to act on yet.
			live.io.pendingJumpSpeed = 0.0f;
			cc.pendingJumpSpeed = 0.0f;
			m_impl->characters.emplace(entity.id, std::move(live));

			const glm::vec3 authoredScale = tc != nullptr ? ExtractScale(tc->localToWorld) : glm::vec3(1.0f);
			reg.emplace_or_replace<PhysicsStateComponent>(enttEntity, pos, rot, pos, rot, authoredScale);
		}
	}

	void PhysicsSystem::StepCharacters(float dt)
	{
		if (m_impl->characters.empty())
		{
			return;
		}
		AE_PROFILE_ZONE_N("Phys.StepCharacters");

		for (auto& [entityId, live]: m_impl->characters)
		{
			(void) entityId;
			JPH::CharacterVirtual* const character = live.character.GetPtr();
			CharacterControllerComponent& io = live.io;

			// Shape rebuild on an authored radius/halfHeight change (inspector/MCP edit).
			// FLT_MAX skips the post-switch penetration check: the new capsule is never
			// smaller everywhere than the old one in a way that matters for this game, and
			// refusing the switch would leave the character on a shape that no longer
			// matches what FlushPendingCharacters just told the caller it has.
			if (live.builtRadius != io.radius || live.builtHalfHeight != io.halfHeight)
			{
				if (const JPH::ShapeRefC shape = MakeCharacterCapsuleShape(io.halfHeight, io.radius))
				{
					character->SetShape(shape, FLT_MAX, m_impl->physics->GetDefaultBroadPhaseLayerFilter(Layers::kMoving), m_impl->physics->GetDefaultLayerFilter(Layers::kMoving), {}, {}, *m_impl->tempAllocator);
					// SetShape does not update the inner rigid body's shape by itself
					// (see SetInnerBodyShape's own doc comment) - without this the
					// character's own collision and the inner body's presence for
					// everyone else silently drift apart on a live radius/halfHeight edit.
					character->SetInnerBodyShape(shape);
					live.builtRadius = io.radius;
					live.builtHalfHeight = io.halfHeight;
				}
			}

			character->SetMaxSlopeAngle(io.maxSlopeAngle);
			character->SetMass(io.mass);
			character->SetMaxStrength(io.maxPushForce);

			if (!io.locallySimulated)
			{
				// Position already mirrored from the replicated transform by
				// FlushPendingCharacters this frame - nothing to integrate.
				io.isGrounded = false;
				io.groundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
				io.velocity = glm::vec3(0.0f);
				continue;
			}

			const JPH::Vec3 up = character->GetUp();
			const JPH::Vec3 gravity = m_impl->physics->GetGravity() * io.gravityScale;
			const JPH::Vec3 currentVelocity = character->GetLinearVelocity();
			const JPH::Vec3 verticalVelocity = up * currentVelocity.Dot(up);
			const JPH::Vec3 groundVelocity = character->GetGroundVelocity();
			const bool onGround = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
			// Matches Jolt's own CharacterVirtual sample: 0.1 m/s of tolerance so ordinary
			// floating-point noise in the ground velocity estimate doesn't flicker the
			// character between "assume ground velocity" and "keep falling" every frame.
			const bool movingTowardsGround = (currentVelocity - groundVelocity).Dot(up) < 0.1f;

			JPH::Vec3 newVelocity;
			if (onGround && movingTowardsGround)
			{
				newVelocity = groundVelocity;
				if (io.pendingJumpSpeed > 0.0f)
				{
					newVelocity += up * io.pendingJumpSpeed;
				}
			}
			else
			{
				newVelocity = verticalVelocity;
			}
			// Consumed whether or not it was actually grounded to catch it: no jump
			// buffering / coyote time. A jump requested a frame too early is simply lost,
			// same as walking into a wall a frame too early does not queue the walk.
			io.pendingJumpSpeed = 0.0f;

			newVelocity += gravity * dt;

			if (io.velocityOverride)
			{
				// Script asked to fully own velocity this step (Physics-style SetVelocity) -
				// replaces the gravity/ground-follow velocity computed above outright.
				newVelocity = ToJolt(io.desiredVelocity);
			}
			else
			{
				// Move()-style horizontal input: strip any vertical component the caller
				// supplied (by mistake or otherwise) so it can never fight the vertical
				// velocity gravity/jump/ground-follow just computed.
				JPH::Vec3 desired = ToJolt(io.desiredVelocity);
				desired -= up * desired.Dot(up);
				newVelocity += desired;
			}

			character->SetLinearVelocity(newVelocity);

			JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
			updateSettings.mStickToFloorStepDown = -up * io.groundSnapDistance;
			updateSettings.mWalkStairsStepUp = up * io.stepHeight;

			character->ExtendedUpdate(dt, gravity, updateSettings,
			        m_impl->physics->GetDefaultBroadPhaseLayerFilter(Layers::kMoving),
			        m_impl->physics->GetDefaultLayerFilter(Layers::kMoving),
			        {},
			        {},
			        *m_impl->tempAllocator);

			io.isGrounded = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
			io.groundNormal = FromJolt(character->GetGroundNormal());
			io.velocity = FromJolt(character->GetLinearVelocity());
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

	// Same body-lookup/creation logic as FlushPendingJoints, over ScriptJointsComponent's
	// vector instead of JointComponent's single field set, and keyed by the handle
	// CreateFixedConstraint/CreateDistanceConstraint already minted (via `entry.created`,
	// not `entry.handle != 0`, since the handle exists from the moment script asked for
	// it - see JointEntry's own comment).
	void PhysicsSystem::FlushPendingScriptJoints(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.FlushScriptJoints");
		auto& reg = world.GetRegistry();
		auto& physics = *m_impl->physics;
		const JPH::BodyLockInterface& bli = physics.GetBodyLockInterface();

		for (auto&& [enttE, comp]: reg.view<ScriptJointsComponent>().each())
		{
			for (JointEntry& entry: comp.joints)
			{
				if (entry.created)
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
				if (entry.target.IsValid() && reg.valid(World::ToEntt(entry.target)))
				{
					const auto* rbOther = reg.try_get<RigidBodyComponent>(World::ToEntt(entry.target));
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
						created = CreateJointConstraint(entry, lock.GetBody(), JPH::Body::sFixedToWorld);
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
						created = CreateJointConstraint(entry, *b1, *b2);
						if (created != nullptr && !entry.collideConnected)
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
				Impl::LiveConstraint live;
				live.constraint = created;
				live.bodyA = selfId.GetIndex();
				live.bodyB = otherId.IsInvalid() ? 0u : otherId.GetIndex();
				live.collisionDisabled = collisionDisabled;
				m_impl->constraints.emplace(entry.handle, std::move(live));
				entry.created = true;
			}
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
		if (!m_impl->constraints.empty())
		{
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

		// ScriptJointsComponent entries must be ERASED, not merely zeroed, or
		// FlushPendingScriptJoints would try forever to build a joint against a body
		// that no longer exists. Runs regardless of whether anything has actually
		// been created in Jolt yet - a still-PENDING entry naming a dying target is
		// exactly as wrong to leave behind as a live one, and a script holding that
		// entry's handle must see DestroyConstraint on it as the documented no-op
		// rather than reach a stale owner.
		for (auto&& [je, comp]: registry.view<ScriptJointsComponent>().each())
		{
			std::erase_if(comp.joints,
			        [&](const JointEntry& entry)
			        {
				        const bool touches = je == enttEntity || (entry.target.IsValid() && World::ToEntt(entry.target) == enttEntity);
				        if (!touches)
				        {
					        return false;
				        }
				        if (entry.created)
				        {
					        RemoveJointConstraint(entry.handle);
				        }
				        m_impl->scriptJointOwners.erase(entry.handle);
				        return true;
			        });
		}
	}

	// Belt-and-suspenders alongside DestroyJointsTouching's own scan: covers this
	// entity's OWN entries the moment ScriptJointsComponent itself is removed or the
	// entity dies, independent of whichever component's on_destroy signal entt
	// happens to fire first during whole-entity destruction.
	void PhysicsSystem::OnScriptJointsDestroyed(entt::registry& registry, entt::entity enttEntity)
	{
		WaitForStep();
		auto* comp = registry.try_get<ScriptJointsComponent>(enttEntity);
		if (comp == nullptr)
		{
			return;
		}
		for (const JointEntry& entry: comp->joints)
		{
			if (entry.created)
			{
				RemoveJointConstraint(entry.handle);
			}
			m_impl->scriptJointOwners.erase(entry.handle);
		}
	}

	std::uint32_t PhysicsSystem::AddScriptJoint(World& world, Entity self, JointEntry entry)
	{
		if (!self.IsValid() || !world.GetRegistry().valid(World::ToEntt(self)))
		{
			return 0;
		}
		entry.handle = m_impl->nextConstraintId++;
		entry.created = false;
		world.GetRegistry().get_or_emplace<ScriptJointsComponent>(World::ToEntt(self)).joints.push_back(entry);
		m_impl->scriptJointOwners.emplace(entry.handle, self);
		return entry.handle;
	}

	std::uint32_t PhysicsSystem::CreateFixedConstraint(World& world, Entity self, Entity target)
	{
		JointEntry entry;
		entry.type = JointType::Fixed;
		entry.target = target;
		return AddScriptJoint(world, self, entry);
	}

	std::uint32_t PhysicsSystem::CreateDistanceConstraint(World& world, Entity self, Entity target, glm::vec3 worldAnchor, float restLength)
	{
		JointEntry entry;
		entry.type = JointType::Distance;
		entry.target = target;
		entry.anchor = worldAnchor;
		entry.distance = std::max(restLength, 0.0f);
		return AddScriptJoint(world, self, entry);
	}

	void PhysicsSystem::DestroyConstraint(World& world, std::uint32_t handle)
	{
		const auto ownerIt = m_impl->scriptJointOwners.find(handle);
		if (ownerIt == m_impl->scriptJointOwners.end())
		{
			return; // unknown or already-destroyed handle - documented no-op, see the header
		}
		const Entity owner = ownerIt->second;
		WaitForStep();
		RemoveJointConstraint(handle); // no-op if FlushPendingScriptJoints never actually created it
		m_impl->scriptJointOwners.erase(ownerIt);
		if (!world.GetRegistry().valid(World::ToEntt(owner)))
		{
			return;
		}
		auto* comp = world.TryGet<ScriptJointsComponent>(owner);
		if (comp == nullptr)
		{
			return;
		}
		std::erase_if(comp->joints, [handle](const JointEntry& e) { return e.handle == handle; });
		if (comp->joints.empty())
		{
			world.Remove<ScriptJointsComponent>(owner);
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

	void PhysicsSystem::OnCharacterControllerDestroyed([[maybe_unused]] entt::registry& registry, entt::entity enttEntity)
	{
		WaitForStep();
		m_impl->characters.erase(World::FromEntt(enttEntity).id);
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

	void PhysicsSystem::AddImpulseAtPoint(PhysicsBodyHandle body, glm::vec3 impulse, glm::vec3 worldPoint)
	{
		WaitForStep();
		const JPH::BodyID id = ToJolt(body);
		if (id.IsInvalid())
		{
			return;
		}
		m_impl->physics->GetBodyInterfaceNoLock().AddImpulse(id, ToJolt(impulse), JPH::RVec3(worldPoint.x, worldPoint.y, worldPoint.z));
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

	void PhysicsSystem::SetBodyMotionType(World& world, Entity entity, PhysicsMotionType motionType)
	{
		auto* rb = world.TryGet<RigidBodyComponent>(entity);
		if (rb == nullptr)
		{
			return;
		}
		// Update the component FIRST: FlushPendingBodies reads rb->motionType when it
		// bakes a body, so a call that arrives before the body exists (a Door script's
		// OnAttach, which runs the same frame the body is first queued) is DEFERRED
		// through the component rather than dropped - the body is created directly in
		// the requested state on the next flush.
		rb->motionType = motionType;
		if (!rb->body.IsValid())
		{
			return;
		}
		WaitForStep();
		const JPH::BodyID id = ToJolt(rb->body);
		auto& bodyInterface = m_impl->physics->GetBodyInterfaceNoLock();
		// rb->body.IsValid() only means "not our own kInvalidValue sentinel". It does
		// NOT mean the handle names a live body: a body can be destroyed and recreated
		// within a frame (RebuildBody on collider field change -> immediate re-bake),
		// and a component that dodged one of those writes keeps the OLD handle - same
		// index, STALE sequence number. BodyInterface::IsAdded() only checks that the
		// INDEX is registered - the recreated body occupies it - so it passes while
		// the handle is still wrong, and handing Jolt a sequence-stale BodyID crashed
		// (0xC0000005 inside BodyInterface::SetMotionType, live-reproduced). A
		// BodyLock validates index AND sequence: Succeeded() is the actual "this
		// handle names the body that is really there" check.
		// A scope, because BodyLockRead is non-assignable and the lock must be
		// released before anything below touches the body or the broadphase.
		JPH::EMotionType currentMotion = JPH::EMotionType::Static;
		{
			JPH::BodyLockRead lock(m_impl->physics->GetBodyLockInterface(), id);
			if (!lock.Succeeded())
			{
				// Self-heal rather than just refuse: drop the stale handle so the next
				// FlushPendingBodies re-bakes a real body directly from rb->motionType
				// (already updated above). Refusing alone would leave the component
				// pointing at a dead body forever, and PushKinematicTargets would keep
				// driving the corpse.
				AE_WARN(LogCategory::Engine,
				        "PhysicsSystem: SetBodyMotionType on entity {} found a stale body handle (index {} sequence-mismatched or never baked); dropped it - a fresh body will be baked from the requested motion type on the next flush",
				        entity.id, id.GetIndex());
				rb->body = {};
				return;
			}
			currentMotion = lock.GetBody().GetMotionType();
		}
		const JPH::EMotionType requestedMotion = ToJoltMotionType(motionType);
		if (currentMotion == JPH::EMotionType::Static && requestedMotion != JPH::EMotionType::Static)
		{
			// Static→Dynamic/Kinematic CANNOT be done in place on a body that was
			// baked as Static: static bodies carry no MotionProperties, and Jolt's own
			// SetMotionType activates the body on this transition before motion
			// properties exist - ActivateBodies -> Body::ResetSleepTimer ->
			// MotionProperties::ResetSleepTestSpheres dereferences null and AVs
			// (0xC0000005, live-verified under a debugger on this exact stack). Route
			// the transition through a full rebuild instead: destroy the body here;
			// rb->motionType is already the requested one, so the next
			// FlushPendingBodies bakes a fresh body with real motion properties in the
			// requested state - the same mechanism a collider-field edit already uses.
			bodyInterface.RemoveBody(id);
			bodyInterface.DestroyBody(id);
			rb->body = {};
			return;
		}
		bodyInterface.SetMotionType(id, requestedMotion, JPH::EActivation::Activate);
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
		FlushPendingScriptJoints(world);
		FlushPendingCharacters(world);
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
