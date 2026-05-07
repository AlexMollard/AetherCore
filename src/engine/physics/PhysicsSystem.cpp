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

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <thread>

#include "physics/PhysicsSystem.hpp"
#include "Components.hpp"
#include "World.hpp"
#include "Logger.hpp"

namespace aether
{

// ── Object / broadphase layer definitions ────────────────────────────────────

namespace Layers
{
	static constexpr JPH::ObjectLayer kNonMoving = 0;
	static constexpr JPH::ObjectLayer kMoving    = 1;
	static constexpr JPH::ObjectLayer kSensor    = 2;
	static constexpr uint32_t         kNumLayers = 3;
}

namespace BroadPhaseLayers
{
	static constexpr JPH::BroadPhaseLayer kNonMoving{ 0 };
	static constexpr JPH::BroadPhaseLayer kMoving{ 1 };
	static constexpr uint32_t             kNumLayers = 2;
}

// ── PhysicsSystem internal Jolt interface implementations ────────────────────

struct PhysicsSystem::BPLayerInterface final : public JPH::BroadPhaseLayerInterface
{
	uint32_t GetNumBroadPhaseLayers() const override
	{
		return BroadPhaseLayers::kNumLayers;
	}

	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
	{
		switch (layer)
		{
			case Layers::kNonMoving: return BroadPhaseLayers::kNonMoving;
			case Layers::kMoving:    return BroadPhaseLayers::kMoving;
			case Layers::kSensor:    return BroadPhaseLayers::kMoving;
			default: JPH_ASSERT(false); return BroadPhaseLayers::kNonMoving;
		}
	}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
	{
		switch ((JPH::BroadPhaseLayer::Type)layer)
		{
			case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::kNonMoving: return "NonMoving";
			case (JPH::BroadPhaseLayer::Type)BroadPhaseLayers::kMoving:    return "Moving";
			default: return "Invalid";
		}
	}
#endif
};

struct PhysicsSystem::ObjVsBPLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
	bool ShouldCollide(JPH::ObjectLayer object, JPH::BroadPhaseLayer bp) const override
	{
		switch (object)
		{
			case Layers::kNonMoving: return bp == BroadPhaseLayers::kMoving;
			case Layers::kMoving:    return true;
			case Layers::kSensor:    return bp == BroadPhaseLayers::kMoving;
			default:                 return false;
		}
	}
};

struct PhysicsSystem::ObjVsObjLayerFilter final : public JPH::ObjectLayerPairFilter
{
	bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
	{
		switch (a)
		{
			case Layers::kNonMoving: return b == Layers::kMoving;
			case Layers::kMoving:    return true; // Moving collides with everything
			case Layers::kSensor:    return b == Layers::kMoving; // Sensors detect movers only
			default:                 return false;
		}
	}
};

// ── Helpers ───────────────────────────────────────────────────────────────────

static JPH::ObjectLayer ToJoltLayer(PhysicsLayer layer)
{
	switch (layer)
	{
		case PhysicsLayer::NonMoving: return Layers::kNonMoving;
		case PhysicsLayer::Moving:    return Layers::kMoving;
		case PhysicsLayer::Sensor:    return Layers::kSensor;
	}
	return Layers::kMoving;
}

static JPH::EMotionType ToJoltMotionType(PhysicsMotionType t)
{
	switch (t)
	{
		case PhysicsMotionType::Static:    return JPH::EMotionType::Static;
		case PhysicsMotionType::Kinematic: return JPH::EMotionType::Kinematic;
		case PhysicsMotionType::Dynamic:   return JPH::EMotionType::Dynamic;
	}
	return JPH::EMotionType::Dynamic;
}

static glm::vec3 FromJolt(JPH::Vec3Arg v)   { return { v.GetX(), v.GetY(), v.GetZ() }; }
static glm::quat FromJolt(JPH::QuatArg  q)  { return { q.GetW(), q.GetX(), q.GetY(), q.GetZ() }; }
static JPH::Vec3 ToJolt(glm::vec3 v)        { return { v.x, v.y, v.z }; }
static JPH::Quat ToJolt(glm::quat q)        { return { q.x, q.y, q.z, q.w }; }

static glm::mat4 ToTransform(glm::vec3 pos, glm::quat rot)
{
	return glm::translate(glm::mat4(1.f), pos) * glm::mat4_cast(rot);
}

// ── PhysicsSystem ─────────────────────────────────────────────────────────────

PhysicsSystem::PhysicsSystem()  = default;
PhysicsSystem::~PhysicsSystem() = default;

void PhysicsSystem::OnRegister([[maybe_unused]] World& world)
{
	JPH::RegisterDefaultAllocator();

	JPH::Factory::sInstance = new JPH::Factory();
	JPH::RegisterTypes();

	// 10 MB scratch for per-step allocations; does not persist between steps.
	m_tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10u * 1024u * 1024u);

	// One worker thread per logical CPU minus the calling thread.
	const int workerThreads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
	m_jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
		JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workerThreads);

	m_bpLayerInterface  = std::make_unique<BPLayerInterface>();
	m_objVsBPFilter     = std::make_unique<ObjVsBPLayerFilter>();
	m_objVsObjFilter    = std::make_unique<ObjVsObjLayerFilter>();

	m_physics = std::make_unique<JPH::PhysicsSystem>();
	m_physics->Init(
		/*maxBodies*/          65'536,
		/*numBodyMutexes*/     0,      // 0 = auto
		/*maxBodyPairs*/       65'536,
		/*maxContactConstraints*/ 10'240,
		*m_bpLayerInterface,
		*m_objVsBPFilter,
		*m_objVsObjFilter);

	m_physics->SetGravity(JPH::Vec3(0.f, -9.81f, 0.f));

	INFO(LogCategory::Engine, "PhysicsSystem initialised (Jolt, {} worker threads, fixed dt = {:.4f} s)",
		workerThreads, kFixedTimestep);
}

void PhysicsSystem::OnUnregister([[maybe_unused]] World& world)
{
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

void PhysicsSystem::OptimizeBroadPhase()
{
	m_physics->OptimizeBroadPhase();
}

// ── Fixed-step update ─────────────────────────────────────────────────────────

void PhysicsSystem::Update(World& world, float dt)
{
	m_accumulator += dt;

	while (m_accumulator >= kFixedTimestep)
	{
		// Save previous state before stepping - used by SyncTransforms for interpolation.
		for (auto [entity, state] : world.View<PhysicsStateComponent>().each())
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
	// collision_steps = 1 is fine for most games at 60 Hz.
	m_physics->Update(kFixedTimestep, /*collision_steps*/ 1, m_tempAllocator.get(), m_jobSystem.get());
}

void PhysicsSystem::SyncTransforms(World& world, float alpha)
{
	auto& bodyInterface = m_physics->GetBodyInterface();

	for (auto [entity, rigid, state, transform] :
		world.View<RigidBodyComponent, PhysicsStateComponent, TransformComponent>().each())
	{
		if (rigid.bodyId.IsInvalid())
			continue;

		// Read current physics state.
		const auto pos = bodyInterface.GetCenterOfMassPosition(rigid.bodyId);
		const auto rot = bodyInterface.GetRotation(rigid.bodyId);
		state.currPosition = FromJolt(pos);
		state.currRotation = FromJolt(rot);

		// Interpolate for smooth rendering at any framerate.
		const glm::vec3 renderPos = glm::mix(state.prevPosition, state.currPosition, alpha);
		const glm::quat renderRot = glm::slerp(state.prevRotation, state.currRotation, alpha);

		transform.localToWorld = ToTransform(renderPos, renderRot);
	}
}

// ── Body factory ──────────────────────────────────────────────────────────────

static glm::vec3 ExtractPosition(const TransformComponent& t)
{
	return { t.localToWorld[3][0], t.localToWorld[3][1], t.localToWorld[3][2] };
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

static void AddBodyToEntity(
	World& world, Entity entity,
	JPH::BodyInterface& bodyInterface,
	JPH::BodyCreationSettings& settings,
	PhysicsMotionType motionType,
	bool startActive)
{
	const JPH::Body* body = bodyInterface.CreateBody(settings);
	if (!body)
	{
		WARN(LogCategory::Engine, "PhysicsSystem: failed to create body - max body limit reached");
		return;
	}

	const JPH::BodyID id = body->GetID();
	bodyInterface.AddBody(id, startActive ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

	world.Emplace<RigidBodyComponent>(entity, id, motionType);

	// Seed both prev and curr to current position so there's no initial interpolation pop.
	const glm::vec3 pos = FromJolt(bodyInterface.GetCenterOfMassPosition(id));
	const glm::quat rot = FromJolt(bodyInterface.GetRotation(id));
	world.Emplace<PhysicsStateComponent>(entity, pos, rot, pos, rot);
}

void PhysicsSystem::AddBoxBody(World& world, Entity entity, BoxBodySettings s)
{
	auto& bodyInterface = m_physics->GetBodyInterface();
	const auto* tc = world.TryGet<TransformComponent>(entity);

	JPH::BoxShapeSettings shapeSettings{ ToJolt(s.halfExtents) };
	shapeSettings.mMaterial = nullptr;
	auto shapeResult = shapeSettings.Create();
	if (shapeResult.HasError())
	{
		WARN(LogCategory::Engine, "PhysicsSystem::AddBoxBody: shape error: {}", shapeResult.GetError().c_str());
		return;
	}

	const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
	const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);

	JPH::BodyCreationSettings bcs{
		shapeResult.Get(),
		JPH::RVec3(pos.x, pos.y, pos.z),
		ToJolt(rot),
		ToJoltMotionType(s.motionType),
		ToJoltLayer(s.layer)
	};
	bcs.mFriction    = s.friction;
	bcs.mRestitution = s.restitution;

	AddBodyToEntity(world, entity, bodyInterface, bcs, s.motionType, s.startActive);
}

void PhysicsSystem::AddSphereBody(World& world, Entity entity, SphereBodySettings s)
{
	auto& bodyInterface = m_physics->GetBodyInterface();
	const auto* tc = world.TryGet<TransformComponent>(entity);

	JPH::SphereShapeSettings shapeSettings{ s.radius };
	auto shapeResult = shapeSettings.Create();
	if (shapeResult.HasError())
	{
		WARN(LogCategory::Engine, "PhysicsSystem::AddSphereBody: shape error: {}", shapeResult.GetError().c_str());
		return;
	}

	const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
	const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);

	JPH::BodyCreationSettings bcs{
		shapeResult.Get(),
		JPH::RVec3(pos.x, pos.y, pos.z),
		ToJolt(rot),
		ToJoltMotionType(s.motionType),
		ToJoltLayer(s.layer)
	};
	bcs.mFriction    = s.friction;
	bcs.mRestitution = s.restitution;

	AddBodyToEntity(world, entity, bodyInterface, bcs, s.motionType, s.startActive);
}

void PhysicsSystem::AddCapsuleBody(World& world, Entity entity, CapsuleBodySettings s)
{
	auto& bodyInterface = m_physics->GetBodyInterface();
	const auto* tc = world.TryGet<TransformComponent>(entity);

	JPH::CapsuleShapeSettings shapeSettings{ s.halfHeight, s.radius };
	auto shapeResult = shapeSettings.Create();
	if (shapeResult.HasError())
	{
		WARN(LogCategory::Engine, "PhysicsSystem::AddCapsuleBody: shape error: {}", shapeResult.GetError().c_str());
		return;
	}

	const glm::vec3 pos = tc ? ExtractPosition(*tc) : glm::vec3(0.f);
	const glm::quat rot = tc ? ExtractRotation(*tc) : glm::quat(1.f, 0.f, 0.f, 0.f);

	JPH::BodyCreationSettings bcs{
		shapeResult.Get(),
		JPH::RVec3(pos.x, pos.y, pos.z),
		ToJolt(rot),
		ToJoltMotionType(s.motionType),
		ToJoltLayer(s.layer)
	};
	bcs.mFriction    = s.friction;
	bcs.mRestitution = s.restitution;

	AddBodyToEntity(world, entity, bodyInterface, bcs, s.motionType, s.startActive);
}

void PhysicsSystem::RemoveBody(World& world, Entity entity)
{
	auto* rigid = world.TryGet<RigidBodyComponent>(entity);
	if (!rigid || rigid->bodyId.IsInvalid())
		return;

	auto& bodyInterface = m_physics->GetBodyInterface();
	bodyInterface.RemoveBody(rigid->bodyId);
	bodyInterface.DestroyBody(rigid->bodyId);

	world.Remove<RigidBodyComponent>(entity);
	world.Remove<PhysicsStateComponent>(entity);
}

// ── Body control ──────────────────────────────────────────────────────────────

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

} // namespace aether
