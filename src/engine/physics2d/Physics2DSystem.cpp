#include "physics2d/Physics2DSystem.hpp"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
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
#include "utils/Hash.hpp"
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

		// A one-way tile shape carries its blocking direction (TileOneWay, 1..4) in
		// its shape user data - a tag pointer, never dereferenced. None (0) shapes
		// leave user data null. Real pointers never fall in 1..4, so the pre-solve
		// can tell a one-way shape from a normal one by this value alone.
		void* OneWayUserData(TileOneWay dir)
		{
			return reinterpret_cast<void*>(static_cast<std::uintptr_t>(dir));
		}

		// One-way pre-solve: called on the physics thread for contacts involving a
		// one-way tile shape. Keep the contact only when the other body is on the
		// solid side (approaching that face); from any other side it passes straight
		// through. b2Manifold.normal points from shape A to shape B, so orient it by
		// which shape is the platform, then project onto the solid-face normal.
		// Entity -> seconds left of a "let me fall through one-way platforms" request.
		// Handed to the pre-solve callback as its context (see OnRegister).
		using DropThroughMap = std::unordered_map<std::uint32_t, float>;

		bool OneWayPreSolve(b2ShapeId shapeIdA, b2ShapeId shapeIdB, b2Manifold* manifold, void* context)
		{
			const auto tag = [](b2ShapeId shape) -> TileOneWay
			{
				const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(b2Shape_GetUserData(shape));
				return (v >= 1 && v <= 4) ? static_cast<TileOneWay>(v) : TileOneWay::None;
			};
			const TileOneWay dirA = tag(shapeIdA);
			const TileOneWay dirB = tag(shapeIdB);
			if ((dirA != TileOneWay::None) == (dirB != TileOneWay::None))
			{
				return true; // neither (or both) is one-way: solve normally
			}
			const bool platformIsA = dirA != TileOneWay::None;

			// Drop-through: while the rider has an active request, this platform is not
			// there at all, so a held Down + jump falls cleanly instead of landing again.
			if (context != nullptr)
			{
				const auto* drops = static_cast<const DropThroughMap*>(context);
				const auto it = drops->find(UnpackEntity(b2Shape_GetBody(platformIsA ? shapeIdB : shapeIdA)));
				if (it != drops->end() && it->second > 0.0f)
				{
					return false;
				}
			}

			const float sign = platformIsA ? 1.0f : -1.0f; // orient normal platform -> body
			const glm::vec2 solid = OneWaySolidNormal(platformIsA ? dirA : dirB);
			return (sign * manifold->normal.x) * solid.x + (sign * manifold->normal.y) * solid.y > 0.5f;
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
				hash = utils::HashCombine(hash, std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.chunkX)) << 32u) | static_cast<std::uint32_t>(key.chunkY)));
				return hash;
			}
		};
		struct TileBodyEntry
		{
			std::uint64_t body = 0; // packed b2BodyId
			std::uint32_t builtRevision = 0;
			// Tileset edits (per-tile collision kind/rect) rebuild without any
			// chunk changing: the store bumps this on save/mutation.
			std::uint32_t builtTileSetGeneration = 0;
			// Chain outlines sample up to two cells into neighbouring chunks
			// (seam ghost vertices), so a neighbour edit must rebuild us too.
			std::uint64_t builtNeighbourRevisions = 0;
			glm::mat4 builtTransform{1.0f};
			bool seen = false;
			// World-space collision geometry for the physics debug overlay
			// (chain polylines, and closed loops for rect-collision tiles).
			std::vector<std::vector<glm::vec2>> debugOutlines;
			// Parallel to debugOutlines: the one-way direction of each outline
			// (None for solid chains / two-way boxes) so the overlay can flag them.
			std::vector<TileOneWay> debugOneWay;
		};
	} // namespace

	struct Physics2DSystem::Impl
	{
		b2WorldId world = b2_nullWorldId;
		std::unordered_map<TileBodyKey, TileBodyEntry, TileBodyKeyHash> tileBodies;
		DropThroughMap dropThrough;
		// Bodies pulled out of the simulation because their hierarchy was
		// disabled, keyed by entity id; SyncTransforms re-enables exactly these.
		std::unordered_set<std::uint32_t> disabledByHierarchy;
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
		// One-way tile platforms disable their contact from below via this callback.
		b2World_SetPreSolveCallback(m_impl->world, &OneWayPreSolve, &m_impl->dropThrough);

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
			// Created under a disabled ancestor: sit out until SyncTransforms sees
			// the disable lifted, mirroring the 3D flush's startActive check.
			if (ecs::HasDisabledAncestor(world, entity))
			{
				b2Body_Disable(body);
				m_impl->disabledByHierarchy.insert(entity.id);
			}

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
					// Sharp corners are safe again: tile terrain is one-sided
					// chain outlines, so there are no rect seams to snag on.
					const b2Polygon box = b2MakeOffsetBox(std::max(0.5f * collider.size.x * s.x, 0.001f), std::max(0.5f * collider.size.y * s.y, 0.001f), center, b2Rot_identity);
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
		ReconcileStaticBodies(world);

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

		// Expire drop-through requests after they have had the step(s) above to take
		// effect, so a request always survives at least one solve.
		for (auto it = m_impl->dropThrough.begin(); it != m_impl->dropThrough.end();)
		{
			it->second -= dt;
			it = it->second <= 0.0f ? m_impl->dropThrough.erase(it) : std::next(it);
		}
	}

	void Physics2DSystem::SetDropThrough(World& world, Entity entity, float seconds)
	{
		if (!entity.IsValid())
		{
			return;
		}
		if (seconds <= 0.0f)
		{
			m_impl->dropThrough.erase(entity.id);
			return;
		}
		m_impl->dropThrough[entity.id] = seconds;

		// A body resting on the platform has almost certainly gone to sleep, and
		// dropping its contact does not wake it - it would just hang there. Wake it so
		// gravity is applied on the very next step.
		if (const auto* rigid = world.TryGet<RigidBody2DComponent>(entity); rigid != nullptr && rigid->body.IsValid())
		{
			const b2BodyId body = LoadBody(rigid->body);
			if (b2Body_IsValid(body))
			{
				b2Body_SetAwake(body, true);
			}
		}
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

	void Physics2DSystem::ReconcileStaticBodies(World& world)
	{
		// A static body's Box2D transform is only ever set at creation, so if the
		// entity's world transform changes afterwards the body is left behind. That
		// happens for prefab instances (the body is built from the prefab's base
		// pose before the instance position override lands, stranding sensors at the
		// origin) and for editor/script set_transform on a static entity. Push the
		// ECS pose into Box2D whenever it has diverged; the divergence gate keeps
		// stable ground/collider bodies from being churned every frame. Requiring a
		// Collider2DComponent scopes this to standard bodies, excluding tilemap
		// chain bodies (which live on the Tile Map entity without one).
		for (const auto& [enttEntity, rigid, collider, state, transform]:
		     world.View<RigidBody2DComponent, Collider2DComponent, Physics2DStateComponent, TransformComponent>().each())
		{
			(void) collider;
			if (rigid.bodyType != Body2DType::Static || !rigid.body.IsValid())
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
			const b2Vec2 cur = b2Body_GetPosition(body);
			const float targetAngle = glm::radians(eulerDeg.z);
			const float posDelta = std::abs(cur.x - pos.x) + std::abs(cur.y - pos.y);
			const float angleDelta = std::abs(ShortestAngleDelta(b2Rot_GetAngle(b2Body_GetRotation(body)), targetAngle));
			if (posDelta < 1e-4f && angleDelta < 1e-4f)
			{
				continue; // already in place - never wake a stable static body
			}
			b2Body_SetTransform(body, {pos.x, pos.y}, b2MakeRot(targetAngle));
			state.prevPosition = state.currPosition = {pos.x, pos.y};
			state.prevAngle = state.currAngle = targetAngle;
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
			if (!rigid.body.IsValid())
			{
				continue;
			}
			const b2BodyId body = LoadBody(rigid.body);
			if (!b2Body_IsValid(body))
			{
				continue;
			}
			const Entity entity = World::FromEntt(enttEntity);
			// A disabled ancestor pulls the body out of the simulation entirely -
			// unlike the 3D system's DeactivateBody, Box2D's disabled set also
			// removes it from queries - and re-enabling returns it awake.
			if (ecs::HasDisabledAncestor(world, entity))
			{
				if (b2Body_IsEnabled(body))
				{
					b2Body_Disable(body);
					m_impl->disabledByHierarchy.insert(entity.id);
				}
				continue;
			}
			if (m_impl->disabledByHierarchy.erase(entity.id) > 0)
			{
				b2Body_Enable(body);
			}
			if (rigid.bodyType != Body2DType::Dynamic)
			{
				continue;
			}

			const b2Vec2 p = b2Body_GetPosition(body);
			state.currPosition = {p.x, p.y};
			state.currAngle = b2Rot_GetAngle(b2Body_GetRotation(body));

			const glm::vec2 renderPos = glm::mix(state.prevPosition, state.currPosition, alpha);
			const float renderAngle = state.prevAngle + ShortestAngleDelta(state.prevAngle, state.currAngle) * alpha;

			ecs::SetWorldTransform(world, entity, ComposeTransform({renderPos.x, renderPos.y, state.depthZ}, {0.0f, 0.0f, glm::degrees(renderAngle)}, state.scale));
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
			// The disable-tracking entry must not outlive the body it names.
			m_impl->disabledByHierarchy.erase(entity.id);
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
		m_impl->disabledByHierarchy.erase(World::FromEntt(enttEntity).id);
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

			// Palette-indexed collision lookup for this map: Full cells merge
			// into chain outlines; Rect cells become per-run boxes.
			struct PaletteCollision
			{
				TileCollisionKind kind = TileCollisionKind::None;
				TileOneWay oneWay = TileOneWay::None;
				glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
			};
			std::vector<PaletteCollision> paletteCollision(map.tilePalette.size());
			for (std::size_t i = 0; i < map.tilePalette.size(); ++i)
			{
				if (const TileDefinition* tile = tileSet.Find(map.tilePalette[i]))
				{
					paletteCollision[i] = {tile->collision, tile->collision != TileCollisionKind::None ? tile->oneWay : TileOneWay::None, tile->collisionRect};
				}
			}
			const auto collisionOf = [&paletteCollision](std::uint32_t cell) -> const PaletteCollision*
			{
				if (tilecell::Empty(cell))
				{
					return nullptr;
				}
				const std::uint16_t index = tilecell::PaletteIndex(cell);
				return index < paletteCollision.size() ? &paletteCollision[index] : nullptr;
			};
			// Two-way solid cells feed the merged chain outlines. One-way cells are
			// excluded here and become separate top-surface box colliders below.
			const auto isSolid = [&collisionOf](std::uint32_t cell)
			{
				const PaletteCollision* collision = collisionOf(cell);
				return collision != nullptr && collision->kind == TileCollisionKind::Full && collision->oneWay == TileOneWay::None;
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
				// Cell solidity by GLOBAL cell coordinate; outline tracing samples
				// up to two cells past a chunk border for seam ghost vertices.
				const auto solidGlobal = [&](glm::ivec2 cell) { return isSolid(map.GetCell(layerIndex, cell)); };

				for (const auto& [chunkKey, chunk]: layer.chunks)
				{
					std::uint64_t neighbourRevisions = 0;
					for (std::int32_t dy = -1; dy <= 1; ++dy)
					{
						for (std::int32_t dx = -1; dx <= 1; ++dx)
						{
							if (dx == 0 && dy == 0)
							{
								continue;
							}
							if (const auto it = layer.chunks.find(TileChunkKey{chunkKey.x + dx, chunkKey.y + dy}); it != layer.chunks.end())
							{
								neighbourRevisions += it->second.revision;
							}
						}
					}

					const std::uint32_t tileSetGeneration = m_tileAssets->TileSetGeneration(map.tileSetPath);
					const TileBodyKey bodyKey{entity.id, static_cast<std::uint32_t>(layerIndex), chunkKey.x, chunkKey.y};
					TileBodyEntry& entry = m_impl->tileBodies[bodyKey];
					entry.seen = true;
					if (entry.builtRevision == chunk.revision && entry.builtTileSetGeneration == tileSetGeneration && entry.builtNeighbourRevisions == neighbourRevisions && entry.builtTransform == transform.localToWorld && entry.body != 0)
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
					entry.builtTileSetGeneration = tileSetGeneration;
					entry.builtNeighbourRevisions = neighbourRevisions;
					entry.builtTransform = transform.localToWorld;

					const glm::ivec2 chunkCellOrigin{chunkKey.x * kTileChunkSize, chunkKey.y * kTileChunkSize};

					// Box2D validates every vertex against B2_HUGE - 100000 length units, see
					// box2d src/constants.h - and ASSERTS past it, which takes the editor down
					// with a debug break and no message. A single cell painted at an absurd
					// coordinate is enough, and paint_tiles accepts any coordinate. Skip the
					// chunk's collision instead of dying; the tiles still draw.
					constexpr float kBox2DMaxExtent = 100000.0f;
					const float chunkFarCell = static_cast<float>(std::max(std::abs(chunkCellOrigin.x), std::abs(chunkCellOrigin.y)) + kTileChunkSize);
					if (chunkFarCell * map.cellSize >= kBox2DMaxExtent)
					{
						static bool warnedFarChunk = false;
						if (!warnedFarChunk)
						{
							warnedFarChunk = true;
							AE_WARN(LogCategory::Engine,
							        "Tile chunk at cell ({}, {}) is beyond Box2D's {} unit limit, so its collision is skipped. Tiles that far out are almost always a mistaken paint coordinate.",
							        chunkCellOrigin.x, chunkCellOrigin.y, kBox2DMaxExtent);
						}
						continue;
					}
					const std::vector<TileChainPath> outlines = TraceSolidOutlines([&](glm::ivec2 local) { return solidGlobal(chunkCellOrigin + local); });

					// Rect-collision runs: contiguous same-palette Rect cells in a
					// row merge into one box (thin platforms etc.).
					struct RectRun
					{
						glm::vec2 min{0.0f};
						glm::vec2 max{0.0f};
						TileOneWay oneWay = TileOneWay::None;
					};
					// A cell needs a box collider (rather than a merged chain) when it
					// is a Rect sub-cell OR any one-way platform; a one-way Full cell
					// uses the whole-cell rect. Two-way Full cells return invalid here
					// and are covered by the chain outlines instead.
					struct BoxInfo
					{
						bool valid = false;
						glm::vec4 rect{0.0f, 0.0f, 1.0f, 1.0f};
						TileOneWay oneWay = TileOneWay::None;
					};
					const auto boxOf = [&collisionOf](std::uint32_t cell) -> BoxInfo
					{
						const PaletteCollision* c = collisionOf(cell);
						if (c == nullptr || c->kind == TileCollisionKind::None)
						{
							return {};
						}
						if (c->kind == TileCollisionKind::Rect)
						{
							return {true, c->rect, c->oneWay};
						}
						// Full: only one-way Full becomes a box; two-way Full -> chains.
						return c->oneWay != TileOneWay::None ? BoxInfo{true, glm::vec4{0.0f, 0.0f, 1.0f, 1.0f}, c->oneWay} : BoxInfo{};
					};
					std::vector<RectRun> rectRuns;
					for (std::int32_t localY = 0; localY < kTileChunkSize; ++localY)
					{
						for (std::int32_t localX = 0; localX < kTileChunkSize;)
						{
							const BoxInfo box = boxOf(chunk.cells[static_cast<std::size_t>(localY) * kTileChunkSize + localX]);
							if (!box.valid)
							{
								++localX;
								continue;
							}
							// Merge by identical rect VALUE and one-way flag: platform
							// left/mid/right caps are distinct tiles sharing one
							// collision rect and must form a single seamless box, but a
							// one-way run must never merge with a two-way one.
							std::int32_t runEnd = localX + 1;
							while (runEnd < kTileChunkSize)
							{
								const BoxInfo next = boxOf(chunk.cells[static_cast<std::size_t>(localY) * kTileChunkSize + runEnd]);
								if (!next.valid || next.rect != box.rect || next.oneWay != box.oneWay)
								{
									break;
								}
								++runEnd;
							}
							const glm::vec4& r = box.rect;
							const glm::vec2 cellBase{static_cast<float>(chunkCellOrigin.x + localX), static_cast<float>(chunkCellOrigin.y + localY)};
							rectRuns.push_back(RectRun{
							        .min = (cellBase + glm::vec2{r.x, r.y}) * cellSize * s,
							        .max = (glm::vec2{static_cast<float>(chunkCellOrigin.x + runEnd - 1) + r.x + r.z, cellBase.y + r.y + r.w}) * cellSize * s,
							        .oneWay = box.oneWay,
							});
							localX = runEnd;
						}
					}

					entry.debugOutlines.clear();
					entry.debugOneWay.clear();
					if (outlines.empty() && rectRuns.empty())
					{
						continue;
					}

					// Debug outline points are stored in WORLD space (chain/box
					// geometry itself stays body-local; the body carries the
					// entity transform).
					const float bodyAngle = glm::radians(eulerDeg.z);
					const float bodyCos = std::cos(bodyAngle);
					const float bodySin = std::sin(bodyAngle);
					const auto toWorldDebug = [&](glm::vec2 local)
					{
						return glm::vec2{pos.x + local.x * bodyCos - local.y * bodySin, pos.y + local.x * bodySin + local.y * bodyCos};
					};

					b2BodyDef bodyDef = b2DefaultBodyDef();
					bodyDef.type = b2_staticBody;
					bodyDef.position = {pos.x, pos.y};
					bodyDef.rotation = b2MakeRot(glm::radians(eulerDeg.z));
					bodyDef.userData = PackEntity(entity);
					const b2BodyId body = b2CreateBody(m_impl->world, &bodyDef);

					// One-sided chain outlines instead of merged boxes: interior
					// segment joins are ghost-collision free, and seams between
					// chunks are covered by the ghost extensions each side
					// contributes (open chains overlap on their end points).
					std::vector<b2Vec2> points;
					std::vector<b2ShapeId> segments;
					for (const TileChainPath& outline: outlines)
					{
						points.clear();
						points.reserve(outline.points.size());
						std::vector<glm::vec2> debugPoints;
						debugPoints.reserve(outline.points.size() + 1);
						for (const glm::ivec2 corner: outline.points)
						{
							const glm::vec2 local = glm::vec2(chunkCellOrigin + corner) * cellSize * s;
							points.push_back({local.x, local.y});
							debugPoints.push_back(toWorldDebug(local));
						}
						if (outline.isLoop && !debugPoints.empty())
						{
							debugPoints.push_back(debugPoints.front());
						}
						entry.debugOutlines.push_back(std::move(debugPoints));
						entry.debugOneWay.push_back(TileOneWay::None); // solid chains are two-way
						b2ChainDef chainDef = b2DefaultChainDef();
						chainDef.points = points.data();
						chainDef.count = static_cast<int>(points.size());
						chainDef.isLoop = outline.isLoop;
						chainDef.enableSensorEvents = true;
						const b2ChainId chain = b2CreateChain(body, &chainDef);
						// Chain segments default to no contact events; tiles keep
						// emitting them so gameplay collision callbacks still fire.
						segments.resize(static_cast<std::size_t>(b2Chain_GetSegmentCount(chain)));
						b2Chain_GetSegments(chain, segments.data(), static_cast<int>(segments.size()));
						for (const b2ShapeId segment: segments)
						{
							b2Shape_EnableContactEvents(segment, true);
						}
					}

					b2ShapeDef rectShapeDef = b2DefaultShapeDef();
					rectShapeDef.enableContactEvents = true;
					rectShapeDef.enableSensorEvents = true;
					for (const RectRun& run: rectRuns)
					{
						const glm::vec2 centre = (run.min + run.max) * 0.5f;
						const glm::vec2 half = glm::max((run.max - run.min) * 0.5f, glm::vec2{0.001f});
						const b2Polygon box = b2MakeOffsetBox(half.x, half.y, {centre.x, centre.y}, b2Rot_identity);
						// One-way runs opt into pre-solve and carry their direction so
						// OneWayPreSolve can drop contacts from the passable sides.
						b2ShapeDef def = rectShapeDef;
						def.enablePreSolveEvents = run.oneWay != TileOneWay::None;
						def.userData = run.oneWay != TileOneWay::None ? OneWayUserData(run.oneWay) : nullptr;
						b2CreatePolygonShape(body, &def, &box);
						entry.debugOutlines.push_back({toWorldDebug({run.min.x, run.min.y}), toWorldDebug({run.max.x, run.min.y}), toWorldDebug({run.max.x, run.max.y}), toWorldDebug({run.min.x, run.max.y}), toWorldDebug({run.min.x, run.min.y})});
						entry.debugOneWay.push_back(run.oneWay);
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

	void Physics2DSystem::ForEachTileDebugOutline(const std::function<void(const std::vector<glm::vec2>&, TileOneWay)>& callback) const
	{
		for (const auto& [key, entry]: m_impl->tileBodies)
		{
			for (std::size_t i = 0; i < entry.debugOutlines.size(); ++i)
			{
				callback(entry.debugOutlines[i], i < entry.debugOneWay.size() ? entry.debugOneWay[i] : TileOneWay::None);
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

	bool Physics2DSystem::IsWorldPointSolid(World& world, glm::vec2 point) const
	{
		if (m_tileAssets == nullptr)
		{
			return false;
		}
		bool solid = false;
		for (const auto& [enttEntity, component, transform]: world.View<TileMapComponent, TransformComponent>().each())
		{
			if (solid || component.tilemapPath.empty())
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
			if (cellSize <= 0.0f)
			{
				continue;
			}

			// world -> tilemap-local -> cell (matches the editor's tile-picking math).
			const glm::vec2 local = glm::vec2(glm::inverse(transform.localToWorld) * glm::vec4(point, 0.0f, 1.0f));
			const glm::ivec2 cell{static_cast<std::int32_t>(std::floor(local.x / cellSize)), static_cast<std::int32_t>(std::floor(local.y / cellSize))};

			for (std::size_t layerIndex = 0; layerIndex < map.layers.size() && !solid; ++layerIndex)
			{
				if (!map.layers[layerIndex].collision)
				{
					continue;
				}
				const std::uint32_t c = map.GetCell(layerIndex, cell);
				if (tilecell::Empty(c))
				{
					continue;
				}
				const std::uint16_t idx = tilecell::PaletteIndex(c);
				if (idx >= map.tilePalette.size())
				{
					continue;
				}
				const TileDefinition* tile = tileSet.Find(map.tilePalette[idx]);
				if (tile != nullptr && tile->collision == TileCollisionKind::Full && tile->oneWay == TileOneWay::None)
				{
					solid = true;
				}
			}
		}
		return solid;
	}

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
