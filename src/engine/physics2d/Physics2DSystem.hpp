#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include <entt/entt.hpp>

#include "assets/TileSetAsset.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "scene/Entity.hpp"
#include "scene/System.hpp"

namespace aether
{
	class TileAssetStore;
	class World;

	// Box2D-backed 2D simulation. Steps on the game thread (the roadmap keeps the
	// owner thread there initially; the command/result contract stays explicit so
	// stepping can move to a worker later). Render interpolation runs through
	// Physics2DStateComponent, mirroring the 3D PhysicsSystem. Box2D types never
	// leak into this header: handles are packed uint64 ids.
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

		// Tile collision needs the tile assets; wired once at registration.
		void SetTileAssets(TileAssetStore* tiles)
		{
			m_tileAssets = tiles;
		}

		// Teleport an entity's body (and interpolation state) to its current
		// TransformComponent pose. Without this, SyncTransforms writes the old
		// body pose straight back over any externally-set transform.
		void TeleportToTransform(World& world, Entity entity);

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

		// True if a two-way-solid tile covers this world point. Uses the tilemap cell data (not the
		// chain colliders, whose interior is hollow), so it answers correctly deep inside a solid block.
		[[nodiscard]] bool IsWorldPointSolid(World& world, glm::vec2 point) const;

		// Tile collision geometry for the physics debug overlay: invokes the
		// callback with world-space polylines (chain outlines and rect loops).
		void ForEachTileDebugOutline(const std::function<void(const std::vector<glm::vec2>& outline, TileOneWay oneWay)>& callback) const;

		// entt destroy hooks - remove the backing Box2D objects so they never leak.
		void OnRigidBody2DDestroyed(entt::registry& registry, entt::entity enttEntity);
		void OnJoint2DDestroyed(entt::registry& registry, entt::entity enttEntity);

	private:
		void FlushPendingBodies(World& world);
		void FlushPendingJoints(World& world);
		// Static per-chunk bodies from greedy-merged solid cells; rebuilds only
		// chunks whose revision (or entity transform) changed.
		void SyncTileMapCollision(World& world);
		void SavePrevState(World& world);
		void DrainEvents(World& world);
		void SyncTransforms(World& world, float alpha);
		void PushKinematicTargets(World& world);
		// Static bodies are authored-transform authoritative but rarely move: push
		// the ECS pose into Box2D only when it has diverged (e.g. a prefab-instance
		// body built before its instance position was applied, or a set_transform).
		void ReconcileStaticBodies(World& world);

		struct Impl;
		std::unique_ptr<Impl> m_impl;
		TileAssetStore* m_tileAssets = nullptr;

		float m_accumulator = 0.0f;
		float m_lastAlpha = 0.0f;

		entt::scoped_connection m_rigidBodyDestroyConn;
		entt::scoped_connection m_jointDestroyConn;
	};
} // namespace aether
