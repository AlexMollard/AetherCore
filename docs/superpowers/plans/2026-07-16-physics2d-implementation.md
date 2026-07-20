# Physics2D (Roadmap Phase 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Production-quality 2D collision and rigid-body gameplay: Box2D v3 behind an engine-owned, Vulkan- and editor-independent interface, with fixed-step simulation, buffered events, queries, joints, debug drawing, managed scripting, and deterministic replay tests.

**Architecture:** A new `src/engine/physics2d/` area mirrors the existing Jolt-backed 3D `PhysicsSystem` patterns: authored components in a components header, a `Physics2DSystem : System` that owns a Box2D world behind a pImpl, fixed-step accumulation with render interpolation via a runtime state component, entt `on_destroy` hooks for leak-free body/joint teardown, and buffered contact/sensor events drained into a `CollisionEvents2DComponent`. Box2D types never appear in engine headers — handles are packed `uint64_t` (`b2StoreBodyId`/`b2LoadBodyId`). The XY plane is the simulation plane; rotation maps to Z angle; transform Z and scale are preserved through sync.

**Tech Stack:** Box2D v3.1.1 (C API, cross-platform deterministic) via CPM; EnTT world/views; existing reflection (`AE_COMPONENT`), scene serializer, interop (`AE_SCRIPT_API`), and debug-line (`DebugVertex`) infrastructure; doctest.

**Key upstream facts (verified 2026-07-16):**
- Latest Box2D v3 tag is `v3.1.1` (`git ls-remote` on erincatto/box2d).
- Systems only tick in play mode: `Application.cpp:360` calls `World::UpdateSystems` only when `m_playState.IsPlaying()`; edit mode calls explicit preview/flush paths (`Application.cpp:366-373`).
- Engine sources are globbed (`src/engine/CMakeLists.txt:7` `CONFIGURE_DEPENDS`) — new engine/app files need **no CMake source-list edits**; only the new dependency and test-support additions touch CMake.
- Context7's Box2D docs track `main`, which has drifted from v3.1.1 (e.g. `b2Pos` origin params). **Always confirm signatures against the vendored header** `build/<preset>/_deps/box2d-src/include/box2d/box2d.h` after the dependency lands; adjust call sites to the tag's actual API, not the doc snippets.

---

## File Map

| File | Responsibility |
|---|---|
| `CMake/Dependencies.cmake` (modify) | CPM Box2D v3.1.1 package |
| `src/engine/CMakeLists.txt` (modify) | Link `box2d` PRIVATE into Engine |
| `src/engine/physics2d/Physics2DComponents.hpp` (create) | Authored + runtime component structs, no Box2D includes |
| `src/engine/physics2d/Physics2DSystem.hpp` (create) | System interface: lifecycle, body ops, queries, events |
| `src/engine/physics2d/Physics2DSystem.cpp` (create) | Box2D world, body/shape/joint creation, step, sync, events, queries |
| `src/engine/physics2d/Physics2DDebugDraw.hpp/.cpp` (create) | Collider wireframe extraction into `DebugVertex` lines |
| `src/app/scene/reflection/Physics2D.reflect.cpp` (create) | `AE_COMPONENT` registration (drives Inspector/catalog/serializer/MCP) |
| `src/app/Application.cpp` (modify) | Register system + service in the `AETHERCORE_SCENE_APP` block; edit-mode flush |
| `src/app/scripting/interop/Physics2DExports.cpp` (create) | Runtime-safe `AE_SCRIPT_API` exports (no editor deps) |
| `managed/AetherCore/Physics2D.cs` (create) | Managed queries/body control |
| `managed/AetherCore/ComponentRef.cs` (modify) | `RigidBody2DRef`, `Collider2DRef` |
| `tests/physics2d/Physics2DTests.cpp` (create) | Simulation, events, queries, determinism tests |
| `tests/CMakeLists.txt` (modify) | Add `Physics2D.reflect.cpp` to the reflect sources list |
| `docs/superpowers/plans/2026-07-15-2d-engine-roadmap.md` (modify) | Tick Phase 3 boxes as each lands |

Sequencing rule from the roadmap honoured here: managed callbacks (Task 8) land only after the native event boundary is tested (Task 4).

---

### Task 1: Box2D dependency

**Files:**
- Modify: `CMake/Dependencies.cmake` (after the Jolt block, ~line 213)
- Modify: `src/engine/CMakeLists.txt` (link libraries)
- Test: `tests/physics2d/Physics2DTests.cpp` (new dir; the tests glob at `tests/CMakeLists.txt:4` is recursive, so no test CMake edit)

- [ ] **Step 1: Add the CPM package**

```cmake
# ── 2D physics ────────────────────────────────────────────────────────────────
# Box2D v3 (C API). Cross-platform deterministic since 3.1 - required for the
# Phase 3 replay tests and future lockstep networking, matching the Jolt policy.
CPMAddPackage(
    NAME box2d
    GIT_REPOSITORY https://github.com/erincatto/box2d.git
    GIT_TAG        v3.1.1
    GIT_SHALLOW    TRUE
    OPTIONS
        "BOX2D_SAMPLES OFF"
        "BOX2D_BENCHMARKS OFF"
        "BOX2D_DOCS OFF"
        "BOX2D_UNIT_TESTS OFF"
        "BOX2D_AVX2 OFF"          # match Jolt: no AVX2 so one binary runs everywhere
)
```

Add `box2d` to the Dependencies solution-folder foreach (`CMake/Dependencies.cmake:281`) and to the root `aethercore_set_folder("Dependencies" ...)` list (`CMakeLists.txt:74`).

- [ ] **Step 2: Link into Engine**

In `src/engine/CMakeLists.txt` `target_link_libraries(Engine ... PRIVATE freetype)` block, add `box2d` to the PRIVATE section. PRIVATE is correct: no engine header includes a Box2D header (handles are packed uint64), and CMake still propagates the archive to Engine's consumers at final link.

- [ ] **Step 3: Write the smoke test**

```cpp
// tests/physics2d/Physics2DTests.cpp
#include <doctest/doctest.h>
#include <box2d/box2d.h>

TEST_CASE("Box2D world steps and a dynamic body falls")
{
	b2WorldDef worldDef = b2DefaultWorldDef();
	worldDef.gravity = {0.0f, -10.0f};
	const b2WorldId world = b2CreateWorld(&worldDef);

	b2BodyDef bodyDef = b2DefaultBodyDef();
	bodyDef.type = b2_dynamicBody;
	bodyDef.position = {0.0f, 10.0f};
	const b2BodyId body = b2CreateBody(world, &bodyDef);
	const b2Polygon box = b2MakeBox(0.5f, 0.5f);
	b2ShapeDef shapeDef = b2DefaultShapeDef();
	b2CreatePolygonShape(body, &shapeDef, &box);

	for (int i = 0; i < 60; ++i)
	{
		b2World_Step(world, 1.0f / 60.0f, 4);
	}

	CHECK(b2Body_GetPosition(body).y < 5.0f);
	b2DestroyWorld(world);
}
```

Note the test target links Engine; Box2D headers must also be visible to the test TU. If the include does not resolve, add `target_include_directories(EngineTests PRIVATE "${box2d_SOURCE_DIR}/include")` in `tests/CMakeLists.txt` (the tests deliberately include Box2D directly only in this smoke test; all other tests go through `Physics2DSystem`).

- [ ] **Step 4: Configure + build + run; fix API drift**

```
cmake --preset default
cmake --build build/default --parallel --target EngineTests
build/default/tests/Debug/EngineTests.exe -tc="Box2D*"
```
Expected: test passes. If any call fails to compile, open `build/default/_deps/box2d-src/include/box2d/` and correct to the v3.1.1 spelling — then keep that spelling for all later tasks.

- [ ] **Step 5: Commit** — `Add Box2D v3.1.1 dependency for Physics2D`

---

### Task 2: Physics2D components + reflection + serialization

**Files:**
- Create: `src/engine/physics2d/Physics2DComponents.hpp`
- Create: `src/app/scene/reflection/Physics2D.reflect.cpp`
- Modify: `tests/CMakeLists.txt` (add the reflect TU next to `Physics.reflect.cpp`, line 16)
- Test: extend `tests/physics2d/Physics2DTests.cpp`

- [ ] **Step 1: Write the components header** (complete file)

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	enum class Body2DType : std::uint8_t
	{
		Static,
		Kinematic,
		Dynamic,
	};

	// Packed b2BodyId (b2StoreBodyId). 0 = invalid: Box2D's null id packs to 0.
	struct Physics2DBodyHandle
	{
		std::uint64_t value = 0;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return value != 0;
		}
	};

	struct RigidBody2DComponent
	{
		Physics2DBodyHandle body; // runtime only - never serialized

		Body2DType bodyType = Body2DType::Dynamic;
		float gravityScale = 1.0f;
		float linearDamping = 0.0f;
		float angularDamping = 0.05f;
		bool fixedRotation = false;
		bool continuousCollision = false; // Box2D "bullet"
		bool allowSleeping = true;
		bool startAwake = true;
		glm::vec2 initialVelocity{0.0f};
		float initialAngularVelocity = 0.0f; // radians/s, +CCW
	};

	enum class Collider2DShape : std::uint8_t
	{
		Box,
		Circle,
		Capsule,
		Polygon,
	};

	struct Collider2DComponent
	{
		Collider2DShape shape = Collider2DShape::Box;
		glm::vec2 size{1.0f, 1.0f}; // box full extents, world units, before entity scale
		float radius = 0.5f;        // circle/capsule
		float capsuleHeight = 1.0f; // capsule end-to-end along local Y, >= 2*radius
		glm::vec2 offset{0.0f};
		float density = 1.0f;
		float friction = 0.5f;
		float restitution = 0.0f;
		bool isTrigger = false;
		std::uint64_t categoryBits = 1;
		std::uint64_t maskBits = ~0ull;
		std::int32_t groupIndex = 0;
		std::vector<glm::vec2> points; // convex polygon verts (max 8, Box2D limit)

		std::vector<std::uint64_t> shapes; // runtime packed b2ShapeIds - never serialized
	};

	// Populated by Physics2DSystem::DrainEvents each fixed step; cleared at the
	// start of each game-thread Update. Never serialized - pure runtime state.
	struct CollisionEvents2DComponent
	{
		std::vector<Entity> collisionEnter;
		std::vector<Entity> collisionExit;
		std::vector<Entity> triggerEnter;
		std::vector<Entity> triggerExit;
		std::vector<Entity> overlapping;
	};

	enum class Joint2DType : std::uint8_t
	{
		Distance,
		Revolute,
		Prismatic,
		Weld,
	};

	struct Joint2DComponent
	{
		Joint2DType type = Joint2DType::Revolute;
		Entity target{};
		glm::vec2 anchorA{0.0f}; // local to this body
		glm::vec2 anchorB{0.0f}; // local to target body
		glm::vec2 axis{1.0f, 0.0f};
		float minLimit = 0.0f; // revolute: radians; prismatic: units
		float maxLimit = 0.0f;
		float length = 1.0f; // distance joint rest length
		float motorSpeed = 0.0f;
		float maxMotorForce = 0.0f;
		bool enableLimit = false;
		bool enableMotor = false;
		bool collideConnected = false;

		std::uint64_t jointId = 0; // packed b2JointId, runtime only
	};

	// Fixed-step interpolation state, mirrors PhysicsStateComponent (3D).
	struct Physics2DStateComponent
	{
		glm::vec2 prevPosition{0.0f};
		float prevAngle = 0.0f;
		glm::vec2 currPosition{0.0f};
		float currAngle = 0.0f;
		float depthZ = 0.0f;    // transform Z preserved through sync
		glm::vec3 scale{1.0f};  // captured at body build; applied on sync
	};
} // namespace aether
```

- [ ] **Step 2: Write the reflection TU** (complete file; mirrors `Physics.reflect.cpp` exactly — field kinds `Float`, `Bool`, `Int`, `Vec2`, `EntityRef`, `AE_FIELD_ENUM` — confirm `Vec2` exists in `Reflection.hpp` field kinds; Phase 1 sprite fields already reflect `glm::vec2` pivots so it does)

```cpp
#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "physics2d/Physics2DComponents.hpp"

using namespace aether;

namespace
{
	const reflect::EnumTable& Body2DTypeEnum()
	{
		static const reflect::EnumTable t{{{"static", static_cast<int>(Body2DType::Static)}, {"kinematic", static_cast<int>(Body2DType::Kinematic)}, {"dynamic", static_cast<int>(Body2DType::Dynamic)}}};
		return t;
	}

	const reflect::EnumTable& Collider2DShapeEnum()
	{
		static const reflect::EnumTable t{{{"box", static_cast<int>(Collider2DShape::Box)}, {"circle", static_cast<int>(Collider2DShape::Circle)}, {"capsule", static_cast<int>(Collider2DShape::Capsule)}, {"polygon", static_cast<int>(Collider2DShape::Polygon)}}};
		return t;
	}

	const reflect::EnumTable& Joint2DTypeEnum()
	{
		static const reflect::EnumTable t{{{"distance", static_cast<int>(Joint2DType::Distance)}, {"revolute", static_cast<int>(Joint2DType::Revolute)}, {"prismatic", static_cast<int>(Joint2DType::Prismatic)}, {"weld", static_cast<int>(Joint2DType::Weld)}}};
		return t;
	}
} // namespace

AE_COMPONENT(RigidBody2DComponent, "Rigid Body 2D", "Physics 2D", ICON_FA_WEIGHT_HANGING)
AE_FIELD_ENUM("body_type", bodyType, Body2DTypeEnum())
AE_FIELD_N("gravity_scale", gravityScale, Float)
AE_FIELD_N("linear_damping", linearDamping, Float)
AE_FIELD_N("angular_damping", angularDamping, Float)
AE_FIELD_N("fixed_rotation", fixedRotation, Bool)
AE_FIELD_N("continuous_collision", continuousCollision, Bool)
AE_FIELD_N("allow_sleeping", allowSleeping, Bool)
AE_FIELD_N("start_awake", startAwake, Bool)
AE_COMPONENT_END()

AE_COMPONENT(Collider2DComponent, "Collider 2D", "Physics 2D", ICON_FA_VECTOR_SQUARE)
AE_FIELD_ENUM("shape", shape, Collider2DShapeEnum())
AE_FIELD_N("size", size, Vec2)
AE_FIELD_N("radius", radius, Float)
AE_FIELD_N("capsule_height", capsuleHeight, Float)
AE_FIELD_N("offset", offset, Vec2)
AE_FIELD_N("density", density, Float)
AE_FIELD_N("friction", friction, Float)
AE_FIELD_N("restitution", restitution, Float)
AE_FIELD_N("is_trigger", isTrigger, Bool)
AE_FIELD_N("group_index", groupIndex, Int)
AE_COMPONENT_END()

AE_COMPONENT(Joint2DComponent, "Joint 2D", "Physics 2D", ICON_FA_LINK)
AE_FIELD_ENUM("type", type, Joint2DTypeEnum())
AE_FIELD_N("target", target, EntityRef)
AE_FIELD_N("anchor_a", anchorA, Vec2)
AE_FIELD_N("anchor_b", anchorB, Vec2)
AE_FIELD_N("axis", axis, Vec2)
AE_FIELD_N("min_limit", minLimit, Float)
AE_FIELD_N("max_limit", maxLimit, Float)
AE_FIELD_N("length", length, Float)
AE_FIELD_N("motor_speed", motorSpeed, Float)
AE_FIELD_N("max_motor_force", maxMotorForce, Float)
AE_FIELD_N("enable_limit", enableLimit, Bool)
AE_FIELD_N("enable_motor", enableMotor, Bool)
AE_FIELD_N("collide_connected", collideConnected, Bool)
AE_COMPONENT_END()
```

Deliberate omissions: `categoryBits`/`maskBits` (uint64 — check whether reflection has a UInt64/Int kind wide enough; if only Int (int32) exists, reflect them later with a bespoke drawer and serialize via Int for now with a comment) and `points` (needs a list drawer — serialized via the polygon path in Task 11). If `Vec2` is genuinely missing as a field kind, add it to `Reflection.hpp` following `Vec3` at every switch site (inspector drawer, TOML read/write, MCP get/set) in this task.

- [ ] **Step 3: Add the reflect TU to the tests reflect-source list** (`tests/CMakeLists.txt` next to line 16's `Physics.reflect.cpp`)

- [ ] **Step 4: Write a serialization round-trip test**

```cpp
TEST_CASE("RigidBody2D and Collider2D survive scene save/load")
{
	// Follow the existing round-trip fixture pattern used by the sprite tests
	// (tests/scene/*): build a World, add an entity with RigidBody2DComponent
	// {bodyType=Kinematic, gravityScale=0.5f, fixedRotation=true} and
	// Collider2DComponent {shape=Capsule, radius=0.25f, capsuleHeight=1.5f,
	// offset={0.1f,-0.2f}, isTrigger=true}, save via SceneSerializer, load into
	// a fresh World, and CHECK every authored field round-trips exactly.
	// Runtime fields (body handle, shapes) must reload as defaults.
}
```

Write it against the real fixture helpers in `tests/scene/` (see how sprite round-trip tests construct serializer deps at `SceneSerializer.cpp:1930`).

- [ ] **Step 5: Build, run `EngineTests.exe -tc="*2D*"`, fix until green**
- [ ] **Step 6: Commit** — `Add Physics2D components, reflection, and serialization`

---

### Task 3: Physics2DSystem core — world, bodies, fixed step, sync

**Files:**
- Create: `src/engine/physics2d/Physics2DSystem.hpp`
- Create: `src/engine/physics2d/Physics2DSystem.cpp`
- Test: extend `tests/physics2d/Physics2DTests.cpp`

- [ ] **Step 1: Write the system header** (complete)

```cpp
#pragma once

#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include "physics2d/Physics2DComponents.hpp"
#include "scene/Entity.hpp"
#include "scene/System.hpp"

namespace aether
{
	class World;

	// Box2D-backed 2D simulation. Steps on the game thread (roadmap: the owner
	// thread is initially the game thread; the command/result contract stays
	// explicit so stepping can move to a worker later). Render interpolation via
	// Physics2DStateComponent, mirroring the 3D PhysicsSystem.
	class Physics2DSystem final : public System
	{
	public:
		Physics2DSystem();
		~Physics2DSystem() override;
		Physics2DSystem(const Physics2DSystem&) = delete;
		Physics2DSystem& operator=(const Physics2DSystem&) = delete;
		Physics2DSystem(Physics2DSystem&&) = delete;
		Physics2DSystem& operator=(Physics2DSystem&&) = delete;

		void OnRegister(World& world) override;
		void Update(World& world, float dt) override;
		void OnUnregister(World& world) override;

		[[nodiscard]] const char* GetName() const override
		{
			return "Physics2DSystem";
		}

		static constexpr float kFixedTimestep = 1.0f / 60.0f;
		static constexpr int kSubStepCount = 4;

		// Editor support: create/destroy backing bodies without stepping
		// (scene load, play/stop restore, inspector edits).
		void FlushPendingOnly(World& world);
		void RebuildBody(World& world, Entity entity);
		void RebuildJoint(World& world, Entity entity);
		void RemoveBody(World& world, Entity entity);

		// Body control (used by interop; safe no-ops on invalid handles).
		void SetLinearVelocity(Physics2DBodyHandle body, glm::vec2 velocity);
		[[nodiscard]] glm::vec2 GetLinearVelocity(Physics2DBodyHandle body) const;
		void SetAngularVelocity(Physics2DBodyHandle body, float radiansPerSec);
		[[nodiscard]] float GetAngularVelocity(Physics2DBodyHandle body) const;
		void ApplyLinearImpulse(Physics2DBodyHandle body, glm::vec2 impulse);
		void ApplyForce(Physics2DBodyHandle body, glm::vec2 force);
		void ApplyTorque(Physics2DBodyHandle body, float torque);
		void ApplyAngularImpulse(Physics2DBodyHandle body, float impulse);
		void SetBodyPosition(Physics2DBodyHandle body, glm::vec2 position);
		void SetBodyAngle(Physics2DBodyHandle body, float radians);
		void SetGravityScale(Physics2DBodyHandle body, float scale);
		void SetBodyAwake(Physics2DBodyHandle body, bool awake);
		[[nodiscard]] bool IsBodyAwake(Physics2DBodyHandle body) const;

		struct RayHit2D
		{
			bool hit = false;
			glm::vec2 point{0.0f};
			glm::vec2 normal{0.0f};
			float fraction = 1.0f;
			std::uint32_t entity = 0;
		};

		// direction must be normalized.
		[[nodiscard]] RayHit2D CastRay(glm::vec2 origin, glm::vec2 direction, float maxDistance) const;
		[[nodiscard]] std::vector<std::uint32_t> OverlapAabb(glm::vec2 min, glm::vec2 max) const;
		[[nodiscard]] std::vector<std::uint32_t> OverlapCircle(glm::vec2 center, float radius) const;
		[[nodiscard]] std::vector<std::uint32_t> OverlapPoint(glm::vec2 point) const;
		[[nodiscard]] RayHit2D CastCircle(glm::vec2 center, float radius, glm::vec2 direction, float maxDistance) const;

		void OnRigidBody2DDestroyed(entt::registry& registry, entt::entity enttEntity);
		void OnJoint2DDestroyed(entt::registry& registry, entt::entity enttEntity);

	private:
		void FlushPendingBodies(World& world);
		void FlushPendingJoints(World& world);
		void SavePrevState(World& world);
		void StepOnce(World& world);
		void DrainEvents(World& world);
		void SyncTransforms(World& world, float alpha);
		void PushKinematicTargets(World& world);

		struct Impl;
		std::unique_ptr<Impl> m_impl;

		float m_accumulator = 0.0f;
		float m_lastAlpha = 0.0f;

		entt::scoped_connection m_rigidBodyDestroyConn;
		entt::scoped_connection m_jointDestroyConn;
	};
} // namespace aether
```

- [ ] **Step 2: Implement the core .cpp**

Complete skeleton with the load-bearing algorithms (fill the remaining trivial setters following the same two-liner shape):

```cpp
#include "physics2d/Physics2DSystem.hpp"

#include <box2d/box2d.h>
#include <glm/gtc/matrix_transform.hpp>

#include "Logging.hpp"
#include "Profile.hpp"
#include "scene/Components.hpp"
#include "scene/EcsHelpers.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

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
				case Body2DType::Static: return b2_staticBody;
				case Body2DType::Kinematic: return b2_kinematicBody;
				case Body2DType::Dynamic:
				default: return b2_dynamicBody;
			}
		}

		// Entity id travels in body user data so query/event results map back.
		void* PackEntity(Entity entity)
		{
			return reinterpret_cast<void*>(static_cast<std::uintptr_t>(entity.Value()));
		}

		std::uint32_t UnpackEntity(b2BodyId body)
		{
			return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(b2Body_GetUserData(body)));
		}
	} // namespace

	struct Physics2DSystem::Impl
	{
		b2WorldId world = b2_nullWorldId;
	};

	Physics2DSystem::Physics2DSystem() : m_impl(std::make_unique<Impl>()) {}
	Physics2DSystem::~Physics2DSystem() = default;

	void Physics2DSystem::OnRegister(World& world)
	{
		b2WorldDef worldDef = b2DefaultWorldDef();
		worldDef.gravity = {0.0f, -9.81f};
		m_impl->world = b2CreateWorld(&worldDef);

		auto& registry = world.Registry();
		m_rigidBodyDestroyConn = registry.on_destroy<RigidBody2DComponent>().connect<&Physics2DSystem::OnRigidBody2DDestroyed>(this);
		m_jointDestroyConn = registry.on_destroy<Joint2DComponent>().connect<&Physics2DSystem::OnJoint2DDestroyed>(this);
	}

	void Physics2DSystem::OnUnregister(World& world)
	{
		m_rigidBodyDestroyConn.release();
		m_jointDestroyConn.release();
		if (b2World_IsValid(m_impl->world))
		{
			b2DestroyWorld(m_impl->world); // destroys all bodies/shapes/joints
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

			glm::vec3 pos{}, eulerDeg{}, scale{};
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
			shapeDef.density = collider.density;
			shapeDef.material.friction = collider.friction;       // v3.1: surface material lives on shapeDef.material
			shapeDef.material.restitution = collider.restitution; // verify exact field names in the vendored header
			shapeDef.isSensor = collider.isTrigger;
			shapeDef.enableContactEvents = !collider.isTrigger;
			shapeDef.enableSensorEvents = true; // sensors AND visitors both need this
			shapeDef.filter.categoryBits = collider.categoryBits;
			shapeDef.filter.maskBits = collider.maskBits;
			shapeDef.filter.groupIndex = collider.groupIndex;

			// Entity scale bakes into the shape geometry (Box2D has no body scale).
			const glm::vec2 s{std::abs(scale.x), std::abs(scale.y)};
			const b2Vec2 center{collider.offset.x * s.x, collider.offset.y * s.y};

			collider.shapes.clear();
			switch (collider.shape)
			{
				case Collider2DShape::Box:
				{
					const b2Polygon box = b2MakeOffsetBox(0.5f * collider.size.x * s.x, 0.5f * collider.size.y * s.y, center, b2Rot_identity);
					collider.shapes.push_back(b2StoreShapeId(b2CreatePolygonShape(body, &shapeDef, &box)));
					break;
				}
				case Collider2DShape::Circle:
				{
					const b2Circle circle{center, collider.radius * std::max(s.x, s.y)};
					collider.shapes.push_back(b2StoreShapeId(b2CreateCircleShape(body, &shapeDef, &circle)));
					break;
				}
				case Collider2DShape::Capsule:
				{
					const float r = collider.radius * s.x;
					const float half = std::max(0.5f * collider.capsuleHeight * s.y - r, 0.001f);
					const b2Capsule capsule{{center.x, center.y - half}, {center.x, center.y + half}, r};
					collider.shapes.push_back(b2StoreShapeId(b2CreateCapsuleShape(body, &shapeDef, &capsule)));
					break;
				}
				case Collider2DShape::Polygon:
				{
					if (collider.points.size() >= 3)
					{
						std::vector<b2Vec2> pts;
						pts.reserve(collider.points.size());
						for (const glm::vec2& p: collider.points)
						{
							pts.push_back({p.x * s.x + center.x, p.y * s.y + center.y});
						}
						const b2Hull hull = b2ComputeHull(pts.data(), static_cast<int>(std::min<std::size_t>(pts.size(), B2_MAX_POLYGON_VERTICES)));
						if (hull.count >= 3)
						{
							const b2Polygon poly = b2MakePolygon(&hull, 0.0f);
							collider.shapes.push_back(b2StoreShapeId(b2CreatePolygonShape(body, &shapeDef, &poly)));
						}
						else
						{
							AE_WARN(LogCategory::Scene, "Collider2D polygon hull degenerate on entity {}", entity.Value());
						}
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

	void Physics2DSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE_N("Physics2D.Update");
		FlushPendingBodies(world);
		FlushPendingJoints(world);

		// Clear per-frame event buffers before accumulating this frame's steps.
		for (const auto& [entity, events]: world.View<CollisionEvents2DComponent>().each())
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
		if (steps == kMaxStepsPerFrame)
		{
			m_accumulator = 0.0f; // drop debt rather than death-spiral
		}

		m_lastAlpha = m_accumulator / kFixedTimestep;
		SyncTransforms(world, m_lastAlpha);
	}

	void Physics2DSystem::SyncTransforms(World& world, float alpha)
	{
		AE_PROFILE_ZONE_N("Physics2D.SyncTransforms");
		for (const auto& [enttEntity, rigid, state, transform]: world.View<RigidBody2DComponent, Physics2DStateComponent, TransformComponent>().each())
		{
			(void) transform;
			if (!rigid.body.IsValid() || rigid.bodyType != Body2DType::Dynamic)
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
			// Shortest-path angle interpolation.
			float delta = state.currAngle - state.prevAngle;
			while (delta > glm::pi<float>()) delta -= glm::two_pi<float>();
			while (delta < -glm::pi<float>()) delta += glm::two_pi<float>();
			const float renderAngle = state.prevAngle + delta * alpha;

			ecs::SetWorldTransform(world, World::FromEntt(enttEntity),
			        ComposeTransform({renderPos.x, renderPos.y, state.depthZ}, {0.0f, 0.0f, glm::degrees(renderAngle)}, state.scale));
		}
	}
} // namespace aether
```

Remaining member functions in this task (all short, follow the shapes above): `SavePrevState` (copy curr→prev for every state component), `PushKinematicTargets` (for kinematic bodies, read ECS transform and `b2Body_SetTransform` — the editor/scripts own kinematic movement), `FlushPendingOnly` (FlushPendingBodies + FlushPendingJoints only), `RemoveBody`/`OnRigidBody2DDestroyed` (destroy `b2BodyId`, clear handle + shapes, remove `Physics2DStateComponent`; body destruction destroys attached shapes), `RebuildBody` (RemoveBody then leave handle invalid so the next flush recreates), and the velocity/impulse/setter wrappers (`b2Body_SetLinearVelocity`, `b2Body_ApplyLinearImpulseToCenter(body, imp, true)`, `b2Body_ApplyForceToCenter`, `b2Body_ApplyTorque`, `b2Body_ApplyAngularImpulse`, `b2Body_SetTransform`, `b2Body_SetGravityScale`, `b2Body_SetAwake`, `b2Body_IsAwake` — each guarded by `b2Body_IsValid`). `FlushPendingJoints`/`RebuildJoint`/`OnJoint2DDestroyed` are stubs until Task 10 (empty bodies with a `// Task 10` comment are acceptable only because the joint component exists and serializes; do not ship Phase 3 without Task 10).

- [ ] **Step 3: Write the simulation tests**

```cpp
#include "physics2d/Physics2DSystem.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "scene/Components.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace
{
	aether::Entity MakeBody2D(aether::World& world, glm::vec3 pos, aether::Body2DType type, aether::Collider2DComponent collider = {})
	{
		const aether::Entity e = world.CreateEntity();
		world.Emplace<aether::TransformComponent>(e, aether::TransformComponent{aether::ComposeTransform(pos, {0, 0, 0}, {1, 1, 1})});
		world.Emplace<aether::RigidBody2DComponent>(e, aether::RigidBody2DComponent{.bodyType = type});
		world.Emplace<aether::Collider2DComponent>(e, std::move(collider));
		return e;
	}

	void StepSeconds(aether::Physics2DSystem& physics, aether::World& world, float seconds)
	{
		const int frames = static_cast<int>(seconds / aether::Physics2DSystem::kFixedTimestep);
		for (int i = 0; i < frames; ++i)
		{
			physics.Update(world, aether::Physics2DSystem::kFixedTimestep);
		}
	}
}

TEST_CASE("Physics2D: dynamic box falls onto static ground and rests")
{
	aether::World world;
	auto system = std::make_unique<aether::Physics2DSystem>();
	auto* physics = system.get();
	world.RegisterSystem(std::move(system));

	MakeBody2D(world, {0.0f, -1.0f, 0.0f}, aether::Body2DType::Static, {.size = {20.0f, 1.0f}});
	const aether::Entity faller = MakeBody2D(world, {0.0f, 5.0f, 3.5f}, aether::Body2DType::Dynamic, {.size = {1.0f, 1.0f}});

	StepSeconds(*physics, world, 3.0f);

	glm::vec3 pos{}, euler{}, scale{};
	aether::DecomposeTRS(world.Get<aether::TransformComponent>(faller).localToWorld, pos, euler, scale);
	CHECK(pos.y == doctest::Approx(0.0f).epsilon(0.1)); // rests on ground top (ground half-height 0.5 + box half 0.5 - 1.0 offset)
	CHECK(pos.z == doctest::Approx(3.5f)); // Z preserved
}
```

Adjust expected rest height once against the actual geometry (ground center -1, half-extent 0.5 → top at -0.5; box half 0.5 → center rests ≈ 0.0). Also add: a `fixedRotation` body keeps angle 0 after an off-center hit, and destroying the entity mid-simulation does not crash the next Update (destroy hook coverage).

- [ ] **Step 4: Run `-tc="Physics2D*"` until green; then run the full suite** (`ctest` — expect 180 pre-existing cases still green)
- [ ] **Step 5: Commit** — `Add Physics2DSystem core: Box2D world, bodies, fixed step, transform sync`

---

### Task 4: Contact and trigger events

**Files:** `Physics2DSystem.cpp` (implement `DrainEvents`), tests.

- [ ] **Step 1: Failing tests first**

```cpp
TEST_CASE("Physics2D: trigger enter/exit events fire once each")
{
	// Static trigger box at origin (isTrigger=true), dynamic box dropped through it.
	// After stepping until pass-through: the trigger entity's CollisionEvents2DComponent
	// accumulated exactly one triggerEnter and one triggerExit for the faller
	// (assert via a per-frame poll that records events before Update clears them),
	// and the dynamic entity got the mirrored events.
}

TEST_CASE("Physics2D: contact begin/end land on both entities")
{
	// Dynamic box dropped onto static ground: both entities see collisionEnter
	// exactly once; 'overlapping' contains the other entity while resting.
}
```

Write these fully (using `MakeBody2D`/`StepSeconds`, polling a captured copy of the events component after each `physics->Update(world, kFixedTimestep)` call).

- [ ] **Step 2: Implement `DrainEvents`**

```cpp
void Physics2DSystem::DrainEvents(World& world)
{
	auto eventsOf = [&world](std::uint32_t rawEntity) -> CollisionEvents2DComponent* {
		const Entity entity{rawEntity};
		if (!world.Valid(entity))
		{
			return nullptr;
		}
		return &world.GetOrEmplace<CollisionEvents2DComponent>(entity);
	};

	const b2ContactEvents contacts = b2World_GetContactEvents(m_impl->world);
	for (int i = 0; i < contacts.beginCount; ++i)
	{
		const b2ContactBeginTouchEvent& e = contacts.beginEvents[i];
		const std::uint32_t a = UnpackEntity(b2Shape_GetBody(e.shapeIdA));
		const std::uint32_t b = UnpackEntity(b2Shape_GetBody(e.shapeIdB));
		if (auto* ev = eventsOf(a)) { ev->collisionEnter.push_back(Entity{b}); ev->overlapping.push_back(Entity{b}); }
		if (auto* ev = eventsOf(b)) { ev->collisionEnter.push_back(Entity{a}); ev->overlapping.push_back(Entity{a}); }
	}
	for (int i = 0; i < contacts.endCount; ++i)
	{
		const b2ContactEndTouchEvent& e = contacts.endEvents[i];
		// End events may reference already-destroyed shapes - validate first.
		if (!b2Shape_IsValid(e.shapeIdA) || !b2Shape_IsValid(e.shapeIdB))
		{
			continue;
		}
		const std::uint32_t a = UnpackEntity(b2Shape_GetBody(e.shapeIdA));
		const std::uint32_t b = UnpackEntity(b2Shape_GetBody(e.shapeIdB));
		if (auto* ev = eventsOf(a)) { ev->collisionExit.push_back(Entity{b}); std::erase(ev->overlapping, Entity{b}); }
		if (auto* ev = eventsOf(b)) { ev->collisionExit.push_back(Entity{a}); std::erase(ev->overlapping, Entity{a}); }
	}

	const b2SensorEvents sensors = b2World_GetSensorEvents(m_impl->world);
	for (int i = 0; i < sensors.beginCount; ++i)
	{
		const b2SensorBeginTouchEvent& e = sensors.beginEvents[i];
		const std::uint32_t sensor = UnpackEntity(b2Shape_GetBody(e.sensorShapeId));
		const std::uint32_t visitor = UnpackEntity(b2Shape_GetBody(e.visitorShapeId));
		if (auto* ev = eventsOf(sensor)) { ev->triggerEnter.push_back(Entity{visitor}); ev->overlapping.push_back(Entity{visitor}); }
		if (auto* ev = eventsOf(visitor)) { ev->triggerEnter.push_back(Entity{sensor}); }
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
		if (auto* ev = eventsOf(sensor)) { ev->triggerExit.push_back(Entity{visitor}); std::erase(ev->overlapping, Entity{visitor}); }
		if (auto* ev = eventsOf(visitor)) { ev->triggerExit.push_back(Entity{sensor}); }
	}
}
```

Scripts consume these after the step on the game thread — callbacks never mutate the Box2D world reentrantly by construction (events are plain component data read next frame).

- [ ] **Step 3: Green, full suite, commit** — `Add Physics2D contact and trigger event buffering`

---

### Task 5: Queries

**Files:** `Physics2DSystem.cpp`, tests.

- [ ] **Step 1: Failing tests** — ray hits a wall and reports entity/point/normal/fraction; ray misses with `hit==false`; `OverlapCircle` finds only bodies inside; `OverlapPoint` inside/outside a box; `CastCircle` stops at the wall.

- [ ] **Step 2: Implement.** `CastRay` via `b2World_CastRayClosest(world, {origin}, {direction * maxDistance}, b2DefaultQueryFilter())` → map `b2RayResult` (`.hit/.point/.normal/.fraction/.shapeId`). Overlaps via `b2World_OverlapAABB` / `b2World_OverlapShape` (`b2MakeProxy`) collecting `UnpackEntity(b2Shape_GetBody(shapeId))` in the callback and returning `true` to continue. `OverlapPoint` = tiny-radius `OverlapCircle` (0.001f). `CastCircle` via `b2World_CastShape` with a fraction-minimizing callback (see the context7 snippet in the plan preamble; validate the v3.1.1 signature — the tag takes no `b2Pos` origin argument).

- [ ] **Step 3: Green, full suite, commit** — `Add Physics2D ray, overlap, point, and shape queries`

---

### Task 6: Determinism replay + behaviour coverage

**Files:** tests only.

- [ ] **Step 1: Replay test** — build the same 30-body mixed scene (boxes/circles/capsules, one trigger, varied restitution) twice in two separate `World`+`Physics2DSystem` instances, step both 600 fixed frames, then CHECK every body's `b2`-reported position and angle is **bitwise identical** (`==` on floats, not Approx — Box2D v3.1 is deterministic for identical inputs on the same binary; the cross-platform contract is recorded in the roadmap, per-platform CI proves it later in Phase 6).
- [ ] **Step 2: Behaviour tests** — bullet (`continuousCollision=true`) body at high speed does not tunnel a thin static wall (and does with a plain body at extreme speed, documenting why bullet exists); collision filters: two bodies with disjoint category/mask pass through; `gravityScale=0` body hangs.
- [ ] **Step 3: Green, commit** — `Add Physics2D determinism replay and CCD/filter coverage`

---

### Task 7: App registration, edit-mode flush, play/stop restore

**Files:**
- Modify: `src/app/Application.cpp` (both the `AETHERCORE_SCENE_APP` registration block ~line 296 and the edit-mode branch ~line 366)

- [ ] **Step 1: Register after the 3D physics system**

```cpp
auto physics2DSystem = std::make_unique<aether::Physics2DSystem>();
auto* physics2DPtr = physics2DSystem.get();
attachContext.Get<World>().RegisterSystem(std::move(physics2DSystem));
services.Register<aether::Physics2DSystem>(*physics2DPtr);
```

- [ ] **Step 2: Edit-mode flush** — in the `else` (not playing) branch next to the 3D physics preview call, add `if (auto* physics2D = ctx.TryGet<aether::Physics2DSystem>()) { physics2D->FlushPendingOnly(ctx.Get<World>()); }` so editor-placed bodies exist for debug draw and queries without simulating.
- [ ] **Step 3: Verify play/stop restore.** Play-mode snapshot restore goes through the scene serializer path (`ResetRestorableEntity` must mirror `ApplySceneToEntities` — see `docs` memory note); because Task 2 registered the components through reflection, they serialize and restore automatically. Manually verify: launch editor, add RigidBody2D+Collider2D to a sprite, Play (it falls), Stop (transform and component values restore). Use the Editor MCP (`create_entity`/`add_component`/`play`/`get_component`) for this check.
- [ ] **Step 4: Confirm inspector: Add Component palette shows "Rigid Body 2D"/"Collider 2D" under "Physics 2D", fields edit and undo cleanly. Body rebuild on edit: hook the inspector's component-changed path the same way 3D colliders do (see `ComponentDrawers.cpp:523` pattern calling `PhysicsSystem` rebuild; add the `Physics2DSystem::RebuildBody` call in the 2D drawer/reflection-edit path).**
- [ ] **Step 5: Full suite + editor smoke, commit** — `Register Physics2DSystem and wire editor flush/rebuild`

---

### Task 8: Managed API

**Files:**
- Create: `src/app/scripting/interop/Physics2DExports.cpp`
- Create: `managed/AetherCore/Physics2D.cs`
- Modify: `managed/AetherCore/ComponentRef.cs`
- Modify: `managed/AetherCore/Events.cs` (only if the 3D collision-event surface needs a 2D twin — inspect how `CollisionEventsComponent` reaches C# and mirror exactly)

- [ ] **Step 1: Exports** — mirror `PhysicsExports.cpp` shapes exactly (`AE_SCRIPT_API`, `ActiveWorld()`, `ActiveContext()`); the context needs a `physics2D` pointer added wherever `ActiveContext().physics` is populated (`InteropCommon.hpp` + its setup site). Functions: `aether_physics2d_add_box(id, Vec2 size, int dynamic)`, `..._add_circle(id, float radius, int dynamic)`, `..._add_capsule(id, float radius, float height, int dynamic)`, `..._set_trigger(id, int)`, `..._set_linear_velocity(id, Vec2)` / `..._get_linear_velocity(id)`, angular pair, `..._apply_impulse(id, Vec2)`, `..._apply_force(id, Vec2)`, `..._apply_torque(id, float)`, `..._set_gravity_scale(id, float)`, `..._raycast(Vec2 origin, Vec2 dir, float maxDist) -> RaycastHit2D{int hit; Vec2 point; Vec2 normal; float fraction; uint entity}`, `..._overlap_circle(Vec2 center, float radius, uint* outBuffer, int capacity) -> int` (buffer-fill pattern — copy whatever `PhysicsExports.cpp`/`WorldExports.cpp` use for array returns). Interop stays runtime-safe: include only engine headers, no `ComponentCatalog`, no ImGui.
- [ ] **Step 2: Managed side** — `Physics2D.cs` static class with `Raycast(Vector2, Vector2, float, out RaycastHit2D)`, `OverlapCircle(Vector2, float) -> Entity[]`; `ComponentRef.cs` gains `RigidBody2DRef`/`Collider2DRef` following `SpriteRendererRef` (lines 47-80) exactly; body-control instance methods (`Velocity`, `AddImpulse`, `GravityScale`, `IsTrigger`) delegating to the exports. Collision/trigger callbacks: mirror the existing 3D mechanism 1:1 (find how `EntityScript` receives `OnCollisionEnter` today; add `OnCollisionEnter2D`/`OnTriggerEnter2D`/exit twins reading `CollisionEvents2DComponent`).
- [ ] **Step 3: Build managed assemblies via the checked-in `AetherCore.sln` path (CMake drives the .csproj build), write a C# smoke script in the test project pattern if one exists; otherwise verify via editor Play with a script that raycasts and logs.**
- [ ] **Step 4: Full suite, commit** — `Add managed Physics2D API and collision callbacks`

---

### Task 9: Debug drawing

**Files:**
- Create: `src/engine/physics2d/Physics2DDebugDraw.hpp/.cpp`
- Modify: the extraction site that already calls `PhysicsDebugRenderer::ExtractShapes`/fills `RenderFramePacket::debugVertices` (find via grep; per the render-extraction invariant, all ECS reads happen in `PrepareFrame` on the game thread)

- [ ] **Step 1:** `void ExtractPhysics2DDebugLines(const World& world, std::vector<DebugVertex>& out)` — walk `Collider2DComponent`+`TransformComponent`, emit wireframes with `AddDebugLine` (box: 4 segments from the OBB corners at the body Z; circle: 24-segment loop; capsule: two half-circles + 2 lines; polygon: closed loop; contacts/sleep state color-coded: awake dynamic = DebugYellow, sleeping = grey tint {0.5,0.5,0.5,1}, trigger = green {0,1,0,1}, static = blue-ish {0.3,0.6,1,1}).
- [ ] **Step 2:** Gate behind the existing `IsPhysicsDebugShapesEnabled()` toggle (same switch the 3D shapes use, so the editor's existing physics-debug menu item drives both).
- [ ] **Step 3:** Editor MCP screenshot with debug shapes on to verify; commit — `Add Physics2D collider debug drawing`

---

### Task 10: Joints

**Files:** `Physics2DSystem.cpp` (`FlushPendingJoints`, `RebuildJoint`, `OnJoint2DDestroyed`), tests.

- [ ] **Step 1: Failing tests** — distance joint holds two dynamic bodies at rest length ±5%; revolute joint with motor spins a body (angle advances with expected sign); destroying the target entity destroys the joint without crashing the next step.
- [ ] **Step 2: Implement.** For each entity with `Joint2DComponent` where `jointId == 0` and both bodies exist: switch on type building `b2DistanceJointDef`/`b2RevoluteJointDef`/`b2PrismaticJointDef`/`b2WeldJointDef` via their `b2Default*JointDef()`, setting `bodyIdA/bodyIdB/localAnchorA/localAnchorB` (anchors are authored in local space already), limits/motor fields per the component, `collideConnected`; create via `b2Create*Joint`, pack with `b2StoreJointId`. Destroy hook: `b2DestroyJoint` if `b2Joint_IsValid`. `RebuildJoint` = destroy + zero id (next flush recreates). Also destroy joints whose target body vanished (validate both body ids in flush; Box2D destroys joints with a body automatically — verify and lean on it).
- [ ] **Step 3: Green, full suite, commit** — `Add Physics2D distance, revolute, prismatic, and weld joints`

---

### Task 11: Sprite outline → collider generation

**Files:**
- Modify: `src/app/scene/SceneSerializer.cpp` only if polygon `points` need explicit TOML (arrays of vec2 — follow how sprite `collisionOutline` persists in `SpriteAtlasAsset.cpp` TOML, but note component TOML goes through reflection; add a bespoke serializer hook for `points` next to whichever component already has one — search the serializer for an existing non-reflected field example and copy it)
- Modify: `src/app/debug/InspectorPanel.cpp` or `ComponentDrawers.cpp`: a "From Sprite Outline" button on the Collider 2D drawer
- Test: unit test for the conversion function

- [ ] **Step 1:** Pure function in `src/engine/physics2d/SpriteColliderGen.hpp/.cpp`:

```cpp
// Converts an authored sprite collision outline (pixel space, origin top-left,
// as stored on SpriteRegion::collisionOutline) into collider local points
// (world units, pivot-relative, +Y up). Returns empty when the outline has
// fewer than 3 points.
std::vector<glm::vec2> BuildColliderPointsFromOutline(std::span<const glm::vec2> outlinePixels, glm::vec2 spritePixelSize, glm::vec2 pivot, float pixelsPerUnit);
```

Implementation: for each pixel point `p`: `x = (p.x - pivot.x * size.x) / ppu`, `y = ((size.y - p.y) - (1 - pivot.y) * size.y) / ppu` — matching the sprite quad math in `SpriteSystem.cpp` (verify against how pivot/UV map there; the unit test pins it: a 32x32 sprite, ppu=32, centered pivot, outline = full rect → points = (±0.5, ±0.5)).
- [ ] **Step 2:** Editor button: reads the entity's `SpriteRendererComponent` atlas + region, calls the function, writes `Collider2DComponent::points`, sets `shape = Polygon`, triggers `RebuildBody`. Warn (existing toast/log path) when the hull drops verts beyond Box2D's 8-vertex cap.
- [ ] **Step 3: Unit test green, editor smoke, commit** — `Generate 2D colliders from sprite collision outlines`

---

### Task 12: Roadmap close-out and validation

- [ ] Tick all Phase 3 checkboxes in `docs/superpowers/plans/2026-07-15-2d-engine-roadmap.md` and update its Status line.
- [ ] Full suite: `ctest -C Debug` (expect old 180 + all new cases green).
- [ ] Reference scenes per the exit gate: build a platformer room + top-down sandbox scene via editor MCP batch ops in the TestingProject; verify behaviour across save/load, play/stop, scripted queries. Screenshot evidence via MCP.
- [ ] Run `superpowers:requesting-code-review` against the accumulated diff; fix findings.
- [ ] Commit — `Complete 2D engine phase 3 Physics2D`

---

## Self-Review Notes

- **Spec coverage:** roadmap Phase 3 items map: Box2D integration→T1/T3; bodies/colliders/filters/triggers/fixed-step/interpolation→T2/T3/T6; event buffering + managed callbacks→T4/T8; queries→T5; collider handles/polygon editing/debug draw→T9/T11 (interactive viewport handles for polygon editing are the one consciously deferred slice — debug draw + inspector editing + outline generation ship first; note this in the roadmap when ticking, or add handles as T11.5 if time allows); joints + CCD→T10/T6; sprite-to-collider→T11; deterministic replay→T6.
- **Types:** `Physics2DBodyHandle.value: uint64`, `RayHit2D`, `Body2DType`, `Collider2DShape`, `Joint2DType` used consistently across tasks; managed names `RigidBody2DRef`/`Collider2DRef` match the roadmap's managed API section.
- **Box2D API risk:** every `b2*` call in this plan must be validated against the vendored v3.1.1 header on first build (Task 1 Step 4) — the doc source tracks main. Known hot spots: `b2ShapeDef.material.friction` vs flat `friction` (changed in 3.1), overlap/cast signatures, `b2ContactEvents` field names, `B2_MAX_POLYGON_VERTICES` spelling.
