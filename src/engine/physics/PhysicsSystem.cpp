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
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ContactListener.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/packing.hpp>

#include <mutex>
#include <thread>
#include <unordered_map>

// Platform thread configuration (name / priority for the physics thread).
#if defined(_WIN32)
#	include <windows.h>
#elif defined(__linux__)
#	include <pthread.h>
#endif

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

	// -- Shape cache helpers -------------------------------------------------------

	static uint64_t BoxKey(glm::vec3 half)
	{
		const auto x = glm::packHalf1x16(half.x);
		const auto y = glm::packHalf1x16(half.y);
		const auto z = glm::packHalf1x16(half.z);
		return (uint64_t(x) << 32) | (uint64_t(y) << 16) | uint64_t(z);
	}

	static uint64_t SphereKey(float radius)
	{
		return glm::packHalf1x16(radius);
	}

	static uint64_t CapsuleKey(float halfHeight, float radius)
	{
		const auto h = glm::packHalf1x16(halfHeight);
		const auto r = glm::packHalf1x16(radius);
		return (uint64_t(h) << 16) | uint64_t(r);
	}

	class JoltRuntime final
	{
	public:
		JoltRuntime()
		{
			std::lock_guard lock(s_mutex);
			if (s_refCount++ == 0)
			{
				JPH::RegisterDefaultAllocator();
				JPH::Factory::sInstance = new JPH::Factory();
				JPH::RegisterTypes();
			}
		}

		~JoltRuntime()
		{
			std::lock_guard lock(s_mutex);
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

	struct PhysicsSystem::Impl
	{
		std::unique_ptr<JoltRuntime> runtime;
		std::unique_ptr<BPLayerInterface> bpLayerInterface;
		std::unique_ptr<ObjVsBPLayerFilter> objVsBPFilter;
		std::unique_ptr<ObjVsObjLayerFilter> objVsObjFilter;
		std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
		std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
		std::unique_ptr<JPH::PhysicsSystem> physics;
		std::unordered_map<uint64_t, JPH::ShapeRefC> shapeCache;
	};

	// -- PhysicsSystem -------------------------------------------------------------

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
			m_stepDone.release(); // signal completion
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

		// 10 MB scratch for per-step allocations; does not persist between steps.
		m_impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10u * 1024u * 1024u);

		// One worker thread per logical CPU minus the calling thread and the
		// dedicated physics thread (which kicks Jolt jobs but doesn't run them).
		const int workerThreads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
		m_impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerThreads);

		m_impl->bpLayerInterface = std::make_unique<BPLayerInterface>();
		m_impl->objVsBPFilter = std::make_unique<ObjVsBPLayerFilter>();
		m_impl->objVsObjFilter = std::make_unique<ObjVsObjLayerFilter>();

		m_impl->physics = std::make_unique<JPH::PhysicsSystem>();
		m_impl->physics->Init(
		        /*maxBodies*/ 64'536,
		        /*numBodyMutexes*/ 0, // 0 = auto
		        /*maxBodyPairs*/ 64'536,
		        /*maxContactConstraints*/ 10'240,
		        *m_impl->bpLayerInterface,
		        *m_impl->objVsBPFilter,
		        *m_impl->objVsObjFilter);

		m_impl->physics->SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));

		// Auto-cleanup: when an entity with RigidBodyComponent is destroyed, the
		// backing physics body is removed and freed so it doesn't leak into the next
		// scene load. This fires for every destruction path (world.Destroy(),
		// registry.remove<RigidBodyComponent>(), etc.).
		m_rigidBodyDestroyConn = world.GetRegistry().on_destroy<RigidBodyComponent>().connect<&PhysicsSystem::OnRigidBodyDestroyed>(this);

		AE_INFO(LogCategory::Engine, "PhysicsSystem initialised (Jolt, {} worker threads, fixed dt = {:.4f} s, dedicated physics thread)", workerThreads, kFixedTimestep);

		StartPhysicsThread();
	}

	void PhysicsSystem::OnUnregister([[maybe_unused]] World& world)
	{
		AE_PROFILE_ZONE();
		StopPhysicsThread();

		m_impl->physics.reset();
		m_impl->shapeCache.clear();
		m_impl->jobSystem.reset();
		m_impl->tempAllocator.reset();
		m_impl->objVsObjFilter.reset();
		m_impl->objVsBPFilter.reset();
		m_impl->bpLayerInterface.reset();
		m_impl->runtime.reset();
	}

	// -- Fixed-step update ---------------------------------------------------------

	void PhysicsSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();

		// 1. Wait for the previous frame's async step to complete.
		WaitForStep();

		// 2. Sync transforms from the completed step.
		//    One frame of latency — same pipelining pattern as RenderThread.
		SyncTransforms(world, m_lastAlpha);

		// 3. Flush pending body descriptors (safe: no step is running).
		FlushPendingBodies(world);

		// 4. Accumulate time and kick step(s) to the physics thread.
		m_accumulator += dt;

		int stepsThisFrame = 0;
		while (m_accumulator >= kFixedTimestep)
		{
			// Extra steps (spiral-of-death catch-up): wait for the in-flight
			// step before saving prev state and kicking the next one.
			if (m_stepInFlight)
			{
				WaitForStep();
			}

			SavePrevState(world);
			m_accumulator -= kFixedTimestep;

			// Kick the step to the physics thread.  The last step in the loop
			// runs async and overlaps with subsequent game-thread work.
			m_stepKick.release();
			m_stepInFlight = true;
			++stepsThisFrame;
		}

		// 5. Save alpha for next frame's sync.
		m_lastAlpha = m_accumulator / kFixedTimestep;

		AE_PROFILE_PLOT("Phys.StepsPerFrame", static_cast<int64_t>(stepsThisFrame));
		AE_PROFILE_PLOT("Phys.AccumulatorMs", static_cast<int64_t>(m_accumulator * 1000.0f));
		AE_PROFILE_PLOT("Phys.RigidBodyCount", static_cast<int64_t>(world.View<RigidBodyComponent>().size()));

		// Step is in flight — other systems now run concurrently with the
		// physics step until WaitForStep() is called (next Update or a physics
		// API call).
	}

	void PhysicsSystem::StepPhysics()
	{
		AE_PROFILE_ZONE_N("Phys.Step");
		AE_PROFILE_PLOT("Phys.TotalBodies", static_cast<int64_t>(m_impl->physics->GetNumBodies()));
		AE_PROFILE_PLOT("Phys.ActiveBodies", static_cast<int64_t>(m_impl->physics->GetNumActiveBodies(JPH::EBodyType::RigidBody)));
		// collision_steps = 1 is fine for most games at 60 Hz.
		m_impl->physics->Update(kFixedTimestep, /*collision_steps*/ 1, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
	}

	void PhysicsSystem::SyncTransforms(World& world, float alpha)
	{
		AE_PROFILE_ZONE_N("Phys.SyncTransforms");
		// WaitForStep() has already run; no step is in flight, NoLock is safe.
		const auto& bi = m_impl->physics->GetBodyInterfaceNoLock();

		int64_t synced = 0;
		for (const auto& [entity, rigid, state, transform]: world.View<RigidBodyComponent, PhysicsStateComponent, TransformComponent>().each())
		{
			const JPH::BodyID id = ToJolt(rigid.body);
			if (id.IsInvalid())
			{
				continue;
			}

			// Skip sleeping bodies — prev == curr, no update needed.
			if (!bi.IsActive(id))
			{
				continue;
			}

			// Read current physics state (single call instead of two).
			JPH::RVec3 pos;
			JPH::Quat rot;
			bi.GetPositionAndRotation(id, pos, rot);
			state.currPosition = FromJolt(pos);
			state.currRotation = FromJolt(rot);

			// Interpolate for smooth rendering at any framerate.
			const glm::vec3 renderPos = glm::mix(state.prevPosition, state.currPosition, alpha);
			const glm::quat renderRot = glm::slerp(state.prevRotation, state.currRotation, alpha);

			transform.localToWorld = ToTransform(renderPos, renderRot, state.scale);
			++synced;
		}

		AE_PROFILE_PLOT("Phys.SyncedBodies", synced);
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

		world.Emplace<RigidBodyComponent>(entity, FromJolt(id), motionType);

		// Seed both prev and curr to current position so there's no initial interpolation pop.
		const glm::vec3 pos = FromJolt(bodyInterface.GetCenterOfMassPosition(id));
		const glm::quat rot = FromJolt(bodyInterface.GetRotation(id));
		world.Emplace<PhysicsStateComponent>(entity, pos, rot, pos, rot, visualScale);

		// Set the initial transform so the app never needs to bake scale manually.
		if (auto tc = world.TryGet<TransformComponent>(entity))
		{
			tc->localToWorld = ToTransform(pos, rot, visualScale);
		}
	}

	void PhysicsSystem::FlushPendingBodies(World& world)
	{
		AE_PROFILE_ZONE_N("Phys.FlushPending");
		auto& bi = m_impl->physics->GetBodyInterfaceNoLock();
		auto& reg = world.GetRegistry();
		bool addedStatic = false;

		const int64_t pendingBox = static_cast<int64_t>(world.View<BoxBodyDesc>().size());
		const int64_t pendingSphere = static_cast<int64_t>(world.View<SphereBodyDesc>().size());
		const int64_t pendingCapsule = static_cast<int64_t>(world.View<CapsuleBodyDesc>().size());
		AE_PROFILE_PLOT("Phys.PendingBox", pendingBox);
		AE_PROFILE_PLOT("Phys.PendingSphere", pendingSphere);
		AE_PROFILE_PLOT("Phys.PendingCapsule", pendingCapsule);

		// World::View returns raw entt views; entities come back as entt::entity.
		// aether::Entity and entt::entity share the same bit representation, so we
		// can reconstruct one from the other inline wherever World's typed API is needed.
		auto toAether = [](entt::entity e) -> Entity
		{
			return Entity{static_cast<uint32_t>(entt::to_integral(e))};
		};

		auto applyVelocity = [&](entt::entity e, glm::vec3 v)
		{
			if (glm::dot(v, v) > 0.f)
			{
				if (const auto r = reg.try_get<RigidBodyComponent>(e))
				{
					bi.SetLinearVelocity(ToJolt(r->body), ToJolt(v));
				}
			}
		};

		// -- Box -------------------------------------------------------------------
		{
			AE_PROFILE_ZONE_N("Phys.Flush.Box");
			for (const auto& [entity, desc]: world.View<BoxBodyDesc>().each())
			{
				if (reg.any_of<RigidBodyComponent>(entity))
				{
					continue;
				}

				const auto tc = reg.try_get<TransformComponent>(entity);
				const uint64_t boxKey = BoxKey(desc.halfExtents);
				auto cachedIt = m_impl->shapeCache.find(boxKey);
				if (cachedIt == m_impl->shapeCache.end())
				{
					JPH::BoxShapeSettings ss{ToJolt(desc.halfExtents)};
					ss.mMaterial = nullptr;
					auto result = ss.Create();
					if (result.HasError())
					{
						AE_WARN(LogCategory::Engine, "PhysicsSystem: box shape error: {}", result.GetError().c_str());
						reg.remove<BoxBodyDesc>(entity);
						continue;
					}
					cachedIt = m_impl->shapeCache.emplace(boxKey, result.Get()).first;
				}

				const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
				const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);
				JPH::BodyCreationSettings bcs{cachedIt->second, JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), ToJoltMotionType(desc.motionType), ToJoltLayer(desc.layer)};
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
		}

		// -- Sphere ----------------------------------------------------------------
		{
			AE_PROFILE_ZONE_N("Phys.Flush.Sphere");
			for (const auto& [entity, desc]: world.View<SphereBodyDesc>().each())
			{
				if (reg.any_of<RigidBodyComponent>(entity))
				{
					continue;
				}

				const auto tc = reg.try_get<TransformComponent>(entity);
				const uint64_t sphereKey = SphereKey(desc.radius);
				auto cachedIt = m_impl->shapeCache.find(sphereKey);
				if (cachedIt == m_impl->shapeCache.end())
				{
					JPH::SphereShapeSettings ss{desc.radius};
					auto result = ss.Create();
					if (result.HasError())
					{
						AE_WARN(LogCategory::Engine, "PhysicsSystem: sphere shape error: {}", result.GetError().c_str());
						reg.remove<SphereBodyDesc>(entity);
						continue;
					}
					cachedIt = m_impl->shapeCache.emplace(sphereKey, result.Get()).first;
				}

				const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
				const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);
				JPH::BodyCreationSettings bcs{cachedIt->second, JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), ToJoltMotionType(desc.motionType), ToJoltLayer(desc.layer)};
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
		}

		// -- Capsule ---------------------------------------------------------------
		{
			AE_PROFILE_ZONE_N("Phys.Flush.Capsule");
			for (const auto& [entity, desc]: world.View<CapsuleBodyDesc>().each())
			{
				if (reg.any_of<RigidBodyComponent>(entity))
				{
					continue;
				}

				const auto tc = reg.try_get<TransformComponent>(entity);
				const uint64_t capsuleKey = CapsuleKey(desc.halfHeight, desc.radius);
				auto cachedIt = m_impl->shapeCache.find(capsuleKey);
				if (cachedIt == m_impl->shapeCache.end())
				{
					JPH::CapsuleShapeSettings ss{desc.halfHeight, desc.radius};
					auto result = ss.Create();
					if (result.HasError())
					{
						AE_WARN(LogCategory::Engine, "PhysicsSystem: capsule shape error: {}", result.GetError().c_str());
						reg.remove<CapsuleBodyDesc>(entity);
						continue;
					}
					cachedIt = m_impl->shapeCache.emplace(capsuleKey, result.Get()).first;
				}

				const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
				const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);
				JPH::BodyCreationSettings bcs{cachedIt->second, JPH::RVec3(pos.x, pos.y, pos.z), ToJolt(rot), ToJoltMotionType(desc.motionType), ToJoltLayer(desc.layer)};
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
		}

		if (addedStatic)
		{
			AE_PROFILE_ZONE_N("Phys.OptimizeBroadPhase");
			m_impl->physics->OptimizeBroadPhase();
		}
	}

	void PhysicsSystem::RemoveBody(World& world, Entity entity)
	{
		WaitForStep();

		auto rigid = world.TryGet<RigidBodyComponent>(entity);
		if (!rigid || !rigid->body.IsValid())
		{
			return;
		}

		auto& bodyInterface = m_impl->physics->GetBodyInterfaceNoLock();
		const JPH::BodyID id = ToJolt(rigid->body);

		// Invalidate BEFORE Remove fires on_destroy — otherwise
		// OnRigidBodyDestroyed re-enters and double-destroys the body.
		rigid->body = {};

		bodyInterface.RemoveBody(id);
		bodyInterface.DestroyBody(id);

		world.Remove<RigidBodyComponent>(entity);
		world.Remove<PhysicsStateComponent>(entity);
		world.Remove<PhysicsDebugShapeComponent>(entity);
	}

	void PhysicsSystem::OnRigidBodyDestroyed(entt::registry& registry, entt::entity enttEntity)
	{
		WaitForStep();

		if (!m_impl->physics)
		{
			return;
		}

		auto rigid = registry.try_get<RigidBodyComponent>(enttEntity);
		if (!rigid || !rigid->body.IsValid())
		{
			return;
		}

		auto& bodyInterface = m_impl->physics->GetBodyInterfaceNoLock();
		const JPH::BodyID id = ToJolt(rigid->body);
		bodyInterface.RemoveBody(id);
		bodyInterface.DestroyBody(id);
	}

	// -- Body control --------------------------------------------------------------

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

	// -- Raycasting --------------------------------------------------------------

	PhysicsSystem::RaycastResult PhysicsSystem::CastRay(glm::vec3 origin, glm::vec3 direction, float maxDistance)
	{
		WaitForStep();

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

		m_impl->physics->GetNarrowPhaseQuery().CastRay(ray, joltResult, JPH::BroadPhaseLayerFilter{}, JPH::ObjectLayerFilter{}, JPH::BodyFilter{});

		if (!joltResult.mBodyID.IsInvalid())
		{
			result.hit = true;
			result.body = FromJolt(joltResult.mBodyID);
			result.fraction = joltResult.mFraction;

			// Hit position: mOrigin + fraction * mDirection (both in RVec3/double).
			JPH::RVec3 hitPosR = ray.GetPointOnRay(joltResult.mFraction);
			result.position = {hitPosR.GetX(), hitPosR.GetY(), hitPosR.GetZ()};

			// Surface normal: lock the body and query the shape.
			JPH::BodyLockRead lock(m_impl->physics->GetBodyLockInterface(), joltResult.mBodyID);
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

} // namespace aether
