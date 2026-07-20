#include "physics2d/Physics2DSystem.hpp"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "assets/TileAssetStore.hpp"
#include "physics2d/PhysicsDomainGate.hpp"
#include "physics2d/TileMapCollision.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	namespace
	{
		b2BodyId LoadBody(Physics2DBodyHandle handle)
		{
			return b2LoadBodyId(handle.value);
		}

		Physics2DBodyHandle StoreBody(b2BodyId id)
		{
			return {b2StoreBodyId(id)};
		}

		b2BodyType ToBox2D(Body2DType type)
		{
			switch (type)
			{
				case Body2DType::Static:
					return b2_staticBody;
				case Body2DType::Kinematic:
					return b2_kinematicBody;
				case Body2DType::Dynamic:
				default:
					return b2_dynamicBody;
			}
		}

		// Entity ids travel in body user data so query/event results map back to
		// ECS entities without any side table.
		void* PackEntity(Entity entity)
		{
			return reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity.id));
		}

		std::uint32_t UnpackEntity(b2BodyId body)
		{
			return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(b2Body_GetUserData(body)));
		}

		CollisionEvents2DComponent* EventsOf(World& world, std::uint32_t rawEntity)
		{
			const Entity entity{rawEntity};
			if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
			{
				return nullptr;
			}
			if (auto* existing = world.TryGet<CollisionEvents2DComponent>(entity))
			{
				return existing;
			}
			return &world.Emplace<CollisionEvents2DComponent>(entity);
		}

		float ShortestAngleDelta(float from, float to)
		{
			float delta = to - from;
			while (delta > glm::pi<float>())
			{
				delta -= glm::two_pi<float>();
			}
			while (delta < -glm::pi<float>())
			{
				delta += glm::two_pi<float>();
			}
			return delta;
		}
	} // namespace

	namespace
	{
		struct TileBodyKey
		{
			std::uint32_t entity = 0;
			std::uint32_t layer = 0;
			std::int32_t chunkX = 0;
			std::int32_t chunkY = 0;
			bool operator==(const TileBodyKey&) const = default;
		};
		struct TileBodyKeyHash
		{
			std::size_t operator()(const TileBodyKey& key) const noexcept
			{
				std::size_t hash = std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(key.entity) << 32u) | key.layer);
				hash ^= std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.chunkX)) << 32u) | static_cast<std::uint32_t>(key.chunkY)) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
				return hash;
			}
		};
		struct TileBodyEntry
		{
			std::uint64_t body = 0; // packed b2BodyId
			std::uint32_t builtRevision = 0;
			glm::mat4 builtTransform{1.0f};
			bool seen = false;
		};
	} // namespace

	struct Physics2DSystem::Impl
	{
		b2WorldId world = b2_nullWorldId;
		std::unordered_map<TileBodyKey, TileBodyEntry, TileBodyKeyHash> tileBodies;
	};

	Physics2DSystem::Physics2DSystem() : m_impl(std::make_unique<Impl>())
	{
	}

	Physics2DSystem::~Physics2DSystem() = default;

	void Physics2DSystem::OnRegister(World& world)
	{
		b2WorldDef worldDef = b2DefaultWorldDef();
		worldDef.gravity = {0.0f, -9.81f};
		m_impl->world = b2CreateWorld(&worldDef);

		m_rigidBodyDestroyConn = world.GetRegistry().on_destroy<RigidBody2DComponent>().connect<&Physics2DSystem::OnRigidBody2DDestroyed>(this);
		m_jointDestroyConn = world.GetRegistry().on_destroy<Joint2DComponent>().connect<&Physics2DSystem::OnJoint2DDestroyed>(this);

		AE_INFO(LogCategory::Engine, "Physics2DSystem initialised (Box2D {}.{}.{}, fixed dt = {:.4f} s, {} sub-steps, game-thread stepping)", b2GetVersion().major, b2GetVersion().minor, b2GetVersion().revision, kFixedTimestep, kSubStepCount);
	}

	void Physics2DSystem::OnUnregister([[maybe_unused]] World& world)
	{
		m_rigidBodyDestroyConn.release();
		m_jointDestroyConn.release();
		if (b2World_IsValid(m_impl->world))
		{
			// Destroys every body, shape, and joint the world still owns.
			b2DestroyWorld(m_impl->world);
			m_impl->world = b2_nullWorldId;
		}
	}

	void Physics2DSystem::FlushPendingBodies(World& world)
	{
		for (const auto& [enttEntity, rigid, collider, transform]: world.View<RigidBody2DComponent, Collider2DComponent, TransformComponent>().each())
		{
			if (rigid.body.IsValid())
			{
				continue;
			}
			const Entity entity = World::FromEntt(enttEntity);
			// One entity never simulates in both domains (hand-edited scenes can
			// still smuggle the combination past the add-time gate).
			if (physics_gate::EntityHas3DPhysics(world, entity))
			{
				AE_WARN(LogCategory::Engine, "Entity {} has both 2D and 3D physics components; skipping its 2D body (remove one set)", entity.id);
				continue;
			}

			glm::vec3 pos{};
			glm::vec3 eulerDeg{};
			glm::vec3 scale{};
			DecomposeTRS(transform.localToWorld, pos, eulerDeg, scale);

			b2BodyDef bodyDef = b2DefaultBodyDef();
			bodyDef.type = ToBox2D(rigid.bodyType);
			bodyDef.position = {pos.x, pos.y};
			bodyDef.rotation = b2MakeRot(glm::radians(eulerDeg.z));
			bodyDef.linearDamping = rigid.linearDamping;
			bodyDef.angularDamping = rigid.angularDamping;
			bodyDef.gravityScale = rigid.gravityScale;
			bodyDef.fixedRotation = rigid.fixedRotation;
			bodyDef.isBullet = rigid.continuousCollision;
			bodyDef.enableSleep = rigid.allowSleeping;
			bodyDef.isAwake = rigid.startAwake;
			bodyDef.linearVelocity = {rigid.initialVelocity.x, rigid.initialVelocity.y};
			bodyDef.angularVelocity = rigid.initialAngularVelocity;
			bodyDef.userData = PackEntity(entity);

			const b2BodyId body = b2CreateBody(m_impl->world, &bodyDef);
			rigid.body = StoreBody(body);

			b2ShapeDef shapeDef = b2DefaultShapeDef();
			shapeDef.density = std::max(collider.density, 0.001f);
			shapeDef.material.friction = collider.friction;
			shapeDef.material.restitution = collider.restitution;
			shapeDef.isSensor = collider.isTrigger;
			shapeDef.enableContactEvents = !collider.isTrigger;
			// Both the sensor and any shape that should be noticed by sensors need
			// sensor events enabled, so every Physics2D shape opts in.
			shapeDef.enableSensorEvents = true;
			shapeDef.filter.categoryBits = collider.categoryBits;
			shapeDef.filter.maskBits = collider.maskBits;
			shapeDef.filter.groupIndex = collider.groupIndex;

			// Box2D has no per-body scale: entity scale bakes into shape geometry.
			const glm::vec2 s{std::max(std::abs(scale.x), 0.001f), std::max(std::abs(scale.y), 0.001f)};
			const b2Vec2 center{collider.offset.x * s.x, collider.offset.y * s.y};

			collider.shapes.clear();
			switch (collider.shape)
			{
				case Collider2DShape::Box:
				{
					const float halfX = std::max(0.5f * collider.size.x * s.x, 0.001f);
					const float halfY = std::max(0.5f * collider.size.y * s.y, 0.001f);
					// Moving boxes get slightly rounded corners so they glide
					// over seams between adjacent static shapes (tile chunk
					// borders) instead of snagging on the ghost corner. Outer
					// dimensions are preserved. Static boxes stay sharp.
					const float round = rigid.bodyType == Body2DType::Static ? 0.0f : std::min({0.04f, halfX * 0.5f, halfY * 0.5f});
					const b2Polygon box = round > 0.0f ? b2MakeOffsetRoundedBox(halfX - round, halfY - round, center, b2Rot_identity, round) : b2MakeOffsetBox(halfX, halfY, center, b2Rot_identity);
					collider.shapes.push_back(b2StoreShapeId(b2CreatePolygonShape(body, &shapeDef, &box)));
					break;
				}
				case Collider2DShape::Circle:
				{
					const b2Circle circle{center, std::max(collider.radius * std::max(s.x, s.y), 0.001f)};
					collider.shapes.push_back(b2StoreShapeId(b2CreateCircleShape(body, &shapeDef, &circle)));
					break;
				}
				case Collider2DShape::Capsule:
				{
					const float radius = std::max(collider.radius * s.x, 0.001f);
					const float half = std::max(0.5f * collider.capsuleHeight * s.y - radius, 0.001f);
					const b2Capsule capsule{{center.x, center.y - half}, {center.x, center.y + half}, radius};
					collider.shapes.push_back(b2StoreShapeId(b2CreateCapsuleShape(body, &shapeDef, &capsule)));
					break;
				}
				case Collider2DShape::Polygon:
				{
					if (collider.points.size() < 3)
					{
						AE_WARN(LogCategory::Engine, "Collider2D polygon on entity {} has {} point(s); at least 3 required - no shape created", entity.id, collider.points.size());
						break;
					}
					std::vector<b2Vec2> pts;
					pts.reserve(collider.points.size());
					for (const glm::vec2& p: collider.points)
					{
						pts.push_back({p.x * s.x + center.x, p.y * s.y + center.y});
					}
					if (pts.size() > B2_MAX_POLYGON_VERTICES)
					{
						AE_WARN(LogCategory::Engine, "Collider2D polygon on entity {} has {} points; Box2D caps convex hulls at {} - extra points are dropped by the hull", entity.id, pts.size(), B2_MAX_POLYGON_VERTICES);
					}
					const b2Hull hull = b2ComputeHull(pts.data(), std::min(static_cast<int>(pts.size()), B2_MAX_POLYGON_VERTICES));
					if (hull.count >= 3)
					{
						const b2Polygon poly = b2MakePolygon(&hull, 0.0f);
						collider.shapes.push_back(b2StoreShapeId(b2CreatePolygonShape(body, &shapeDef, &poly)));
					}
					else
					{
						AE_WARN(LogCategory::Engine, "Collider2D polygon hull degenerate on entity {} - no shape created", entity.id);
					}
					break;
				}
			}

			auto& state = world.EmplaceOrReplace<Physics2DStateComponent>(entity);
			state.prevPosition = state.currPosition = {pos.x, pos.y};
			state.prevAngle = state.currAngle = glm::radians(eulerDeg.z);
			state.depthZ = pos.z;
			state.scale = scale;
		}
	}

	void Physics2DSystem::FlushPendingJoints(World& world)
	{
		for (const auto& [enttEntity, joint]: world.View<Joint2DComponent>().each())
		{
			if (joint.jointId != 0)
			{
				// Box2D destroys joints with their bodies; clear stale ids so a
				// rebuilt body pair gets its joint recreated on this flush.
				if (b2Joint_IsValid(b2LoadJointId(joint.jointId)))
				{
					continue;
				}
				joint.jointId = 0;
			}
			const Entity entity = World::FromEntt(enttEntity);
			if (!joint.target.IsValid())
			{
				continue;
			}
			const auto* rigidA = world.TryGet<RigidBody2DComponent>(joint.target);
			const auto* rigidB = world.TryGet<RigidBody2DComponent>(entity);
			if (rigidA == nullptr || rigidB == nullptr || !rigidA->body.IsValid() || !rigidB->body.IsValid())
			{
				continue; // both bodies must exist first; retried next flush
			}
			const b2BodyId bodyA = LoadBody(rigidA->body);
			const b2BodyId bodyB = LoadBody(rigidB->body);
			if (!b2Body_IsValid(bodyA) || !b2Body_IsValid(bodyB))
			{
				continue;
			}

			b2JointId created = b2_nullJointId;
			switch (joint.type)
			{
				case Joint2DType::Distance:
				{
					b2DistanceJointDef def = b2DefaultDistanceJointDef();
					def.bodyIdA = bodyA;
					def.bodyIdB = bodyB;
					def.localAnchorA = {joint.connectedAnchor.x, joint.connectedAnchor.y};
					def.localAnchorB = {joint.anchor.x, joint.anchor.y};
					def.length = std::max(joint.length, 0.001f);
					def.enableLimit = joint.enableLimit;
					def.minLength = std::max(joint.minLimit, 0.0f);
					def.maxLength = joint.maxLimit > 0.0f ? joint.maxLimit : def.length;
					def.enableMotor = joint.enableMotor;
					def.motorSpeed = joint.motorSpeed;
					def.maxMotorForce = joint.maxMotorForce;
					def.collideConnected = joint.collideConnected;
					created = b2CreateDistanceJoint(m_impl->world, &def);
					break;
				}
				case Joint2DType::Revolute:
				{
					b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
					def.bodyIdA = bodyA;
					def.bodyIdB = bodyB;
					def.localAnchorA = {joint.connectedAnchor.x, joint.connectedAnchor.y};
					def.localAnchorB = {joint.anchor.x, joint.anchor.y};
					def.enableLimit = joint.enableLimit;
					def.lowerAngle = joint.minLimit;
					def.upperAngle = joint.maxLimit;
					def.enableMotor = joint.enableMotor;
					def.motorSpeed = joint.motorSpeed;
					def.maxMotorTorque = joint.maxMotorForce;
					def.collideConnected = joint.collideConnected;
					created = b2CreateRevoluteJoint(m_impl->world, &def);
					break;
				}
				case Joint2DType::Prismatic:
				{
					b2PrismaticJointDef def = b2DefaultPrismaticJointDef();
					def.bodyIdA = bodyA;
					def.bodyIdB = bodyB;
					def.localAnchorA = {joint.connectedAnchor.x, joint.connectedAnchor.y};
					def.localAnchorB = {joint.anchor.x, joint.anchor.y};
					const float axisLength = glm::length(joint.axis);
					const glm::vec2 axis = axisLength > 1e-6f ? joint.axis / axisLength : glm::vec2{1.0f, 0.0f};
					def.localAxisA = {axis.x, axis.y};
					def.enableLimit = joint.enableLimit;
					def.lowerTranslation = joint.minLimit;
					def.upperTranslation = joint.maxLimit;
					def.enableMotor = joint.enableMotor;
					def.motorSpeed = joint.motorSpeed;
					def.maxMotorForce = joint.maxMotorForce;
					def.collideConnected = joint.collideConnected;
					created = b2CreatePrismaticJoint(m_impl->world, &def);
					break;
				}
				case Joint2DType::Weld:
				{
					b2WeldJointDef def = b2DefaultWeldJointDef();
					def.bodyIdA = bodyA;
					def.bodyIdB = bodyB;
					def.localAnchorA = {joint.connectedAnchor.x, joint.connectedAnchor.y};
					def.localAnchorB = {joint.anchor.x, joint.anchor.y};
					def.collideConnected = joint.collideConnected;
					created = b2CreateWeldJoint(m_impl->world, &def);
					break;
				}
			}
			if (!B2_IS_NULL(created))
			{
				joint.jointId = b2StoreJointId(created);
			}
		}
	}

	void Physics2DSystem::FlushPendingOnly(World& world)
	{
		FlushPendingBodies(world);
		FlushPendingJoints(world);
		SyncTileMapCollision(world);
	}

	void Physics2DSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE_N("Physics2D.Update");
		FlushPendingBodies(world);
		FlushPendingJoints(world);
		SyncTileMapCollision(world);

		// Enter/exit buffers are per-game-frame; 'overlapping' persists as the
		// live contact/trigger "stay" set.
		for (const auto& [enttEntity, events]: world.View<CollisionEvents2DComponent>().each())
		{
			events.collisionEnter.clear();
			events.collisionExit.clear();
			events.triggerEnter.clear();
			events.triggerExit.clear();
		}

		PushKinematicTargets(world);

		m_accumulator += dt;
		constexpr int kMaxStepsPerFrame = 8; // spiral-of-death guard
		int steps = 0;
		while (m_accumulator >= kFixedTimestep && steps < kMaxStepsPerFrame)
		{
			SavePrevState(world);
			b2World_Step(m_impl->world, kFixedTimestep, kSubStepCount);
			DrainEvents(world);
			m_accumulator -= kFixedTimestep;
			++steps;
		}
		if (steps == kMaxStepsPerFrame && m_accumulator >= kFixedTimestep)
		{
			AE_WARN(LogCategory::Engine, "Physics2D dropped {:.1f} ms of simulation debt after {} steps in one frame", m_accumulator * 1000.0f, steps);
			m_accumulator = 0.0f;
		}

		m_lastAlpha = std::clamp(m_accumulator / kFixedTimestep, 0.0f, 1.0f);
		SyncTransforms(world, m_lastAlpha);
	}

	void Physics2DSystem::SavePrevState(World& world)
	{
		for (const auto& [enttEntity, state]: world.View<Physics2DStateComponent>().each())
		{
			state.prevPosition = state.currPosition;
			state.prevAngle = state.currAngle;
		}
	}

	void Physics2DSystem::PushKinematicTargets(World& world)
	{
		// Scripts/editor own kinematic transforms: push the ECS pose into Box2D
		// before stepping so kinematic bodies carry authored motion.
		for (const auto& [enttEntity, rigid, state, transform]: world.View<RigidBody2DComponent, Physics2DStateComponent, TransformComponent>().each())
		{
			if (rigid.bodyType != Body2DType::Kinematic || !rigid.body.IsValid())
			{
				continue;
			}
			const b2BodyId body = LoadBody(rigid.body);
			if (!b2Body_IsValid(body))
			{
				continue;
			}
			glm::vec3 pos{};
			glm::vec3 eulerDeg{};
			glm::vec3 scale{};
			DecomposeTRS(transform.localToWorld, pos, eulerDeg, scale);
			b2Body_SetTransform(body, {pos.x, pos.y}, b2MakeRot(glm::radians(eulerDeg.z)));
			state.prevPosition = state.currPosition = {pos.x, pos.y};
			state.prevAngle = state.currAngle = glm::radians(eulerDeg.z);
			state.depthZ = pos.z;
			state.scale = scale;
		}
	}

	void Physics2DSystem::DrainEvents(World& world)
	{
		AE_PROFILE_ZONE_N("Physics2D.DrainEvents");

		const b2ContactEvents contacts = b2World_GetContactEvents(m_impl->world);
		for (int i = 0; i < contacts.beginCount; ++i)
		{
			const b2ContactBeginTouchEvent& e = contacts.beginEvents[i];
			const std::uint32_t a = UnpackEntity(b2Shape_GetBody(e.shapeIdA));
			const std::uint32_t b = UnpackEntity(b2Shape_GetBody(e.shapeIdB));
			if (auto* ev = EventsOf(world, a))
			{
				ev->collisionEnter.push_back(Entity{b});
				ev->overlapping.push_back(Entity{b});
			}
			if (auto* ev = EventsOf(world, b))
			{
				ev->collisionEnter.push_back(Entity{a});
				ev->overlapping.push_back(Entity{a});
			}
		}
		for (int i = 0; i < contacts.endCount; ++i)
		{
			const b2ContactEndTouchEvent& e = contacts.endEvents[i];
			// End events may reference shapes destroyed this step - validate first.
			if (!b2Shape_IsValid(e.shapeIdA) || !b2Shape_IsValid(e.shapeIdB))
			{
				continue;
			}
			const std::uint32_t a = UnpackEntity(b2Shape_GetBody(e.shapeIdA));
			const std::uint32_t b = UnpackEntity(b2Shape_GetBody(e.shapeIdB));
			if (auto* ev = EventsOf(world, a))
			{
				ev->collisionExit.push_back(Entity{b});
				std::erase(ev->overlapping, Entity{b});
			}
			if (auto* ev = EventsOf(world, b))
			{
				ev->collisionExit.push_back(Entity{a});
				std::erase(ev->overlapping, Entity{a});
			}
		}

		const b2SensorEvents sensors = b2World_GetSensorEvents(m_impl->world);
		for (int i = 0; i < sensors.beginCount; ++i)
		{
			const b2SensorBeginTouchEvent& e = sensors.beginEvents[i];
			const std::uint32_t sensor = UnpackEntity(b2Shape_GetBody(e.sensorShapeId));
			const std::uint32_t visitor = UnpackEntity(b2Shape_GetBody(e.visitorShapeId));
			if (auto* ev = EventsOf(world, sensor))
			{
				ev->triggerEnter.push_back(Entity{visitor});
				ev->overlapping.push_back(Entity{visitor});
			}
			if (auto* ev = EventsOf(world, visitor))
			{
				ev->triggerEnter.push_back(Entity{sensor});
			}
		}
		for (int i = 0; i < sensors.endCount; ++i)
		{
			const b2SensorEndTouchEvent& e = sensors.endEvents[i];
			if (!b2Shape_IsValid(e.sensorShapeId) || !b2Shape_IsValid(e.visitorShapeId))
			{
				continue;
			}
			const std::uint32_t sensor = UnpackEntity(b2Shape_GetBody(e.sensorShapeId));
			const std::uint32_t visitor = UnpackEntity(b2Shape_GetBody(e.visitorShapeId));
			if (auto* ev = EventsOf(world, sensor))
			{
				ev->triggerExit.push_back(Entity{visitor});
				std::erase(ev->overlapping, Entity{visitor});
			}
			if (auto* ev = EventsOf(world, visitor))
			{
				ev->triggerExit.push_back(Entity{sensor});
			}
		}
	}

	void Physics2DSystem::SyncTransforms(World& world, float alpha)
	{
		AE_PROFILE_ZONE_N("Physics2D.SyncTransforms");
		std::int64_t synced = 0;
		for (const auto& [enttEntity, rigid, state, transform]: world.View<RigidBody2DComponent, Physics2DStateComponent, TransformComponent>().each())
		{
			(void) transform;
			if (rigid.bodyType != Body2DType::Dynamic || !rigid.body.IsValid())
			{
				continue;
			}
			const b2BodyId body = LoadBody(rigid.body);
			if (!b2Body_IsValid(body))
			{
				continue;
			}
			if (ecs::HasDisabledAncestor(world, World::FromEntt(enttEntity)))
			{
				continue;
			}

			const b2Vec2 p = b2Body_GetPosition(body);
			state.currPosition = {p.x, p.y};
			state.currAngle = b2Rot_GetAngle(b2Body_GetRotation(body));

			const glm::vec2 renderPos = glm::mix(state.prevPosition, state.currPosition, alpha);
			const float renderAngle = state.prevAngle + ShortestAngleDelta(state.prevAngle, state.currAngle) * alpha;

			ecs::SetWorldTransform(world, World::FromEntt(enttEntity), ComposeTransform({renderPos.x, renderPos.y, state.depthZ}, {0.0f, 0.0f, glm::degrees(renderAngle)}, state.scale));
			++synced;
		}
		AE_PROFILE_PLOT("Physics2D.SyncedBodies", synced);
	}

	void Physics2DSystem::RemoveBody(World& world, Entity entity)
	{
		auto* rigid = world.TryGet<RigidBody2DComponent>(entity);
		if (rigid == nullptr)
		{
			return;
		}
		if (rigid->body.IsValid())
		{
			const b2BodyId body = LoadBody(rigid->body);
			if (b2Body_IsValid(body))
			{
				// Destroys the attached shapes and any joints touching the body.
				b2DestroyBody(body);
			}
			rigid->body = {};
		}
		if (auto* collider = world.TryGet<Collider2DComponent>(entity))
		{
			collider->shapes.clear();
		}
		if (world.Has<Physics2DStateComponent>(entity))
		{
			world.Remove<Physics2DStateComponent>(entity);
		}
	}

	void Physics2DSystem::RebuildBody(World& world, Entity entity)
	{
		// Drop the backing body; the next flush recreates it from authored state.
		RemoveBody(world, entity);
		FlushPendingBodies(world);
	}

	void Physics2DSystem::RebuildJoint(World& world, Entity entity)
	{
		auto* joint = world.TryGet<Joint2DComponent>(entity);
		if (joint == nullptr)
		{
			return;
		}
		if (joint->jointId != 0)
		{
			const b2JointId id = b2LoadJointId(joint->jointId);
			if (b2Joint_IsValid(id))
			{
				b2DestroyJoint(id);
			}
			joint->jointId = 0;
		}
		FlushPendingJoints(world);
	}

	void Physics2DSystem::OnRigidBody2DDestroyed(entt::registry& registry, entt::entity enttEntity)
	{
		auto& rigid = registry.get<RigidBody2DComponent>(enttEntity);
		if (!rigid.body.IsValid())
		{
			return;
		}
		const b2BodyId body = LoadBody(rigid.body);
		if (b2Body_IsValid(body))
		{
			b2DestroyBody(body);
		}
		rigid.body = {};
		if (auto* collider = registry.try_get<Collider2DComponent>(enttEntity))
		{
			collider->shapes.clear();
		}
	}

	void Physics2DSystem::OnJoint2DDestroyed(entt::registry& registry, entt::entity enttEntity)
	{
		auto& joint = registry.get<Joint2DComponent>(enttEntity);
		if (joint.jointId == 0)
		{
			return;
		}
		const b2JointId id = b2LoadJointId(joint.jointId);
		if (b2Joint_IsValid(id))
		{
			b2DestroyJoint(id);
		}
		joint.jointId = 0;
	}

	void Physics2DSystem::SyncTileMapCollision(World& world)
	{
		AE_PROFILE_ZONE_N("Physics2D.TileCollision");
		if (m_tileAssets == nullptr || !b2World_IsValid(m_impl->world))
		{
			return;
		}
		for (auto& [key, entry]: m_impl->tileBodies)
		{
			entry.seen = false;
		}

		for (const auto& [enttEntity, component, transform]: world.View<TileMapComponent, TransformComponent>().each())
		{
			if (component.tilemapPath.empty())
			{
				continue;
			}
			const auto mapResult = m_tileAssets->LoadTileMap(component.tilemapPath);
			if (!mapResult.has_value())
			{
				continue;
			}
			const TileMapAsset& map = **mapResult;
			const auto tileSetResult = m_tileAssets->LoadTileSet(map.tileSetPath);
			if (!tileSetResult.has_value())
			{
				continue;
			}
			const TileSetAsset& tileSet = **tileSetResult;
			const float cellSize = map.cellSize > 0.0f ? map.cellSize : tileSet.cellSize;
			const Entity entity = World::FromEntt(enttEntity);

			// Palette-indexed solidity lookup for this map.
			std::vector<std::uint8_t> solidByPalette(map.tilePalette.size(), 0);
			for (std::size_t i = 0; i < map.tilePalette.size(); ++i)
			{
				const TileDefinition* tile = tileSet.Find(map.tilePalette[i]);
				solidByPalette[i] = (tile != nullptr && tile->collision == TileCollisionKind::Full) ? 1 : 0;
			}
			const auto isSolid = [&solidByPalette](std::uint32_t cell)
			{
				if (tilecell::Empty(cell))
				{
					return false;
				}
				const std::uint16_t index = tilecell::PaletteIndex(cell);
				return index < solidByPalette.size() && solidByPalette[index] != 0;
			};

			glm::vec3 pos{};
			glm::vec3 eulerDeg{};
			glm::vec3 scale{};
			DecomposeTRS(transform.localToWorld, pos, eulerDeg, scale);
			const glm::vec2 s{std::max(std::abs(scale.x), 0.001f), std::max(std::abs(scale.y), 0.001f)};

			for (std::size_t layerIndex = 0; layerIndex < map.layers.size(); ++layerIndex)
			{
				const TileMapLayer& layer = map.layers[layerIndex];
				if (!layer.collision)
				{
					continue;
				}
				for (const auto& [chunkKey, chunk]: layer.chunks)
				{
					const TileBodyKey bodyKey{entity.id, static_cast<std::uint32_t>(layerIndex), chunkKey.x, chunkKey.y};
					TileBodyEntry& entry = m_impl->tileBodies[bodyKey];
					entry.seen = true;
					if (entry.builtRevision == chunk.revision && entry.builtTransform == transform.localToWorld && entry.body != 0)
					{
						continue;
					}
					if (entry.body != 0)
					{
						const b2BodyId old = b2LoadBodyId(entry.body);
						if (b2Body_IsValid(old))
						{
							b2DestroyBody(old);
						}
						entry.body = 0;
					}
					entry.builtRevision = chunk.revision;
					entry.builtTransform = transform.localToWorld;

					const std::vector<TileRect> rects = MergeSolidCells(chunk.cells, isSolid);
					if (rects.empty())
					{
						continue;
					}

					b2BodyDef bodyDef = b2DefaultBodyDef();
					bodyDef.type = b2_staticBody;
					bodyDef.position = {pos.x, pos.y};
					bodyDef.rotation = b2MakeRot(glm::radians(eulerDeg.z));
					bodyDef.userData = PackEntity(entity);
					const b2BodyId body = b2CreateBody(m_impl->world, &bodyDef);

					b2ShapeDef shapeDef = b2DefaultShapeDef();
					shapeDef.enableContactEvents = true;
					shapeDef.enableSensorEvents = true;
					const glm::vec2 chunkOrigin{static_cast<float>(chunkKey.x * kTileChunkSize) * cellSize, static_cast<float>(chunkKey.y * kTileChunkSize) * cellSize};
					for (const TileRect& rect: rects)
					{
						const glm::vec2 centre = (chunkOrigin + glm::vec2{(static_cast<float>(rect.x) + static_cast<float>(rect.w) * 0.5f) * cellSize, (static_cast<float>(rect.y) + static_cast<float>(rect.h) * 0.5f) * cellSize}) * s;
						const glm::vec2 half{0.5f * static_cast<float>(rect.w) * cellSize * s.x, 0.5f * static_cast<float>(rect.h) * cellSize * s.y};
						const b2Polygon box = b2MakeOffsetBox(std::max(half.x, 0.001f), std::max(half.y, 0.001f), {centre.x, centre.y}, b2Rot_identity);
						b2CreatePolygonShape(body, &shapeDef, &box);
					}
					entry.body = b2StoreBodyId(body);
				}
			}
		}

		// Drop bodies whose entity/layer/chunk vanished this sync.
		for (auto it = m_impl->tileBodies.begin(); it != m_impl->tileBodies.end();)
		{
			if (!it->second.seen)
			{
				if (it->second.body != 0)
				{
					const b2BodyId body = b2LoadBodyId(it->second.body);
					if (b2Body_IsValid(body))
					{
						b2DestroyBody(body);
					}
				}
				it = m_impl->tileBodies.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	// -- Body control -----------------------------------------------------------

	namespace
	{
		b2BodyId ValidBodyOrNull(Physics2DBodyHandle handle)
		{
			if (!handle.IsValid())
			{
				return b2_nullBodyId;
			}
			const b2BodyId body = b2LoadBodyId(handle.value);
			return b2Body_IsValid(body) ? body : b2_nullBodyId;
		}
	} // namespace

	void Physics2DSystem::SetLinearVelocity(Physics2DBodyHandle handle, glm::vec2 velocity)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_SetLinearVelocity(body, {velocity.x, velocity.y});
			// Unity semantics: writing a non-zero velocity revives a sleeping
			// body. Box2D v3's setter deliberately does not, which silently
			// freezes script-driven actors once they idle long enough to sleep.
			if (velocity.x != 0.0f || velocity.y != 0.0f)
			{
				b2Body_SetAwake(body, true);
			}
		}
	}

	glm::vec2 Physics2DSystem::GetLinearVelocity(Physics2DBodyHandle handle) const
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			const b2Vec2 v = b2Body_GetLinearVelocity(body);
			return {v.x, v.y};
		}
		return {0.0f, 0.0f};
	}

	void Physics2DSystem::SetAngularVelocity(Physics2DBodyHandle handle, float radiansPerSec)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_SetAngularVelocity(body, radiansPerSec);
			if (radiansPerSec != 0.0f)
			{
				b2Body_SetAwake(body, true);
			}
		}
	}

	float Physics2DSystem::GetAngularVelocity(Physics2DBodyHandle handle) const
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			return b2Body_GetAngularVelocity(body);
		}
		return 0.0f;
	}

	void Physics2DSystem::ApplyLinearImpulse(Physics2DBodyHandle handle, glm::vec2 impulse)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_ApplyLinearImpulseToCenter(body, {impulse.x, impulse.y}, true);
		}
	}

	void Physics2DSystem::ApplyForce(Physics2DBodyHandle handle, glm::vec2 force)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_ApplyForceToCenter(body, {force.x, force.y}, true);
		}
	}

	void Physics2DSystem::ApplyTorque(Physics2DBodyHandle handle, float torque)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_ApplyTorque(body, torque, true);
		}
	}

	void Physics2DSystem::ApplyAngularImpulse(Physics2DBodyHandle handle, float impulse)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_ApplyAngularImpulse(body, impulse, true);
		}
	}

	void Physics2DSystem::TeleportToTransform(World& world, Entity entity)
	{
		const auto* transform = world.TryGet<TransformComponent>(entity);
		auto* state = world.TryGet<Physics2DStateComponent>(entity);
		if (transform == nullptr || state == nullptr)
		{
			return;
		}
		glm::vec3 pos{};
		glm::vec3 eulerDeg{};
		glm::vec3 scale{};
		DecomposeTRS(transform->localToWorld, pos, eulerDeg, scale);
		const float angle = glm::radians(eulerDeg.z);
		state->prevPosition = state->currPosition = glm::vec2(pos);
		state->prevAngle = state->currAngle = angle;
		state->depthZ = pos.z;
		state->scale = glm::max(scale, glm::vec3(0.001f));

		if (const auto* rigid = world.TryGet<RigidBody2DComponent>(entity))
		{
			if (const b2BodyId body = ValidBodyOrNull(rigid->body); !B2_IS_NULL(body))
			{
				b2Body_SetTransform(body, {pos.x, pos.y}, b2MakeRot(angle));
				b2Body_SetAwake(body, true);
			}
		}
	}

	void Physics2DSystem::SetBodyPosition(Physics2DBodyHandle handle, glm::vec2 position)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_SetTransform(body, {position.x, position.y}, b2Body_GetRotation(body));
		}
	}

	void Physics2DSystem::SetBodyAngle(Physics2DBodyHandle handle, float radians)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_SetTransform(body, b2Body_GetPosition(body), b2MakeRot(radians));
		}
	}

	void Physics2DSystem::SetGravityScale(Physics2DBodyHandle handle, float scale)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_SetGravityScale(body, scale);
		}
	}

	void Physics2DSystem::SetBodyAwake(Physics2DBodyHandle handle, bool awake)
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			b2Body_SetAwake(body, awake);
		}
	}

	bool Physics2DSystem::IsBodyAwake(Physics2DBodyHandle handle) const
	{
		if (const b2BodyId body = ValidBodyOrNull(handle); !B2_IS_NULL(body))
		{
			return b2Body_IsAwake(body);
		}
		return false;
	}

	// -- Queries ------------------------------------------------------------------

	namespace
	{
		bool CollectOverlapEntities(b2ShapeId shapeId, void* context)
		{
			auto* out = static_cast<std::vector<std::uint32_t>*>(context);
			const std::uint32_t entity = UnpackEntity(b2Shape_GetBody(shapeId));
			if (std::find(out->begin(), out->end(), entity) == out->end())
			{
				out->push_back(entity);
			}
			return true; // continue the query
		}

		struct ClosestCastContext
		{
			b2ShapeId shapeId = b2_nullShapeId;
			b2Vec2 point{};
			b2Vec2 normal{};
			float fraction = 1.0f;
			bool hit = false;
		};

		float ClosestCastCallback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void* context)
		{
			// Ignore initial overlap (a cast starting inside a shape reports
			// fraction 0) - matches Box2D's own closest-hit helper.
			if (fraction == 0.0f)
			{
				return -1.0f;
			}
			// Unity semantics: casts see solid geometry, never triggers. Box2D
			// v3's closest-ray helper reports sensors, so filter here.
			if (b2Shape_IsSensor(shapeId))
			{
				return -1.0f; // ignore and continue
			}
			auto* closest = static_cast<ClosestCastContext*>(context);
			closest->shapeId = shapeId;
			closest->point = point;
			closest->normal = normal;
			closest->fraction = fraction;
			closest->hit = true;
			return fraction; // clip the remaining cast to the closest hit so far
		}
	} // namespace

	Physics2DSystem::RayHit2D Physics2DSystem::CastRay(glm::vec2 origin, glm::vec2 direction, float maxDistance) const
	{
		RayHit2D hit;
		if (!b2World_IsValid(m_impl->world))
		{
			return hit;
		}
		const b2Vec2 translation{direction.x * maxDistance, direction.y * maxDistance};
		ClosestCastContext closest;
		b2World_CastRay(m_impl->world, {origin.x, origin.y}, translation, b2DefaultQueryFilter(), ClosestCastCallback, &closest);
		if (!closest.hit)
		{
			return hit;
		}
		hit.hit = true;
		hit.point = {closest.point.x, closest.point.y};
		hit.normal = {closest.normal.x, closest.normal.y};
		hit.fraction = closest.fraction;
		hit.entity = UnpackEntity(b2Shape_GetBody(closest.shapeId));
		return hit;
	}

	std::vector<std::uint32_t> Physics2DSystem::OverlapAabb(glm::vec2 min, glm::vec2 max) const
	{
		std::vector<std::uint32_t> entities;
		if (!b2World_IsValid(m_impl->world))
		{
			return entities;
		}
		const b2AABB aabb{{min.x, min.y}, {max.x, max.y}};
		b2World_OverlapAABB(m_impl->world, aabb, b2DefaultQueryFilter(), CollectOverlapEntities, &entities);
		return entities;
	}

	std::vector<std::uint32_t> Physics2DSystem::OverlapCircle(glm::vec2 center, float radius) const
	{
		std::vector<std::uint32_t> entities;
		if (!b2World_IsValid(m_impl->world))
		{
			return entities;
		}
		const b2Vec2 c{center.x, center.y};
		const b2ShapeProxy proxy = b2MakeProxy(&c, 1, radius);
		b2World_OverlapShape(m_impl->world, &proxy, b2DefaultQueryFilter(), CollectOverlapEntities, &entities);
		return entities;
	}

	std::vector<std::uint32_t> Physics2DSystem::OverlapPoint(glm::vec2 point) const
	{
		return OverlapCircle(point, 0.001f);
	}

	Physics2DSystem::RayHit2D Physics2DSystem::CastCircle(glm::vec2 center, float radius, glm::vec2 direction, float maxDistance) const
	{
		RayHit2D hit;
		if (!b2World_IsValid(m_impl->world))
		{
			return hit;
		}
		const b2Vec2 c{center.x, center.y};
		const b2ShapeProxy proxy = b2MakeProxy(&c, 1, radius);
		const b2Vec2 translation{direction.x * maxDistance, direction.y * maxDistance};
		ClosestCastContext closest;
		b2World_CastShape(m_impl->world, &proxy, translation, b2DefaultQueryFilter(), ClosestCastCallback, &closest);
		if (!closest.hit)
		{
			return hit;
		}
		hit.hit = true;
		hit.point = {closest.point.x, closest.point.y};
		hit.normal = {closest.normal.x, closest.normal.y};
		hit.fraction = closest.fraction;
		hit.entity = UnpackEntity(b2Shape_GetBody(closest.shapeId));
		return hit;
	}
} // namespace aether
