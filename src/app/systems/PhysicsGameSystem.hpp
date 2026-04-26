#pragma once

#include <glm/glm.hpp>
#include <random>
#include <vector>

#include "AetherCore.hpp"
#include "Camera.hpp"
#include "CameraManager.hpp"
#include "Entity.hpp"
#include "GraphicsPipeline.hpp"
#include "Material.hpp"
#include "System.hpp"
#include "physics/PhysicsSystem.hpp"

namespace aether
{
	class AssetManager;
	class Input;
} // namespace aether

namespace aether::app
{
	// Drives the physics demo scene:
	//   - A static ground plane + static boundary walls
	//   - A stack of boxes and a scattering of spheres that fall and collide
	//   - A "cannon" that fires a heavy sphere on Space
	//   - R resets the scene, C swaps camera
	//
	// All bodies use fixed-step Jolt simulation (60 Hz) with render interpolation,
	// so the scene is frame-rate-independent and deterministic — ready for networking.
	class PhysicsGameSystem final : public aether::System
	{
	public:
		void Init(aether::AetherCore& engine, aether::AssetManager& assets,
		          aether::CameraManager& cameras, aether::Input& input,
		          aether::PhysicsSystem& physics);

		const char* GetName() const override { return "PhysicsGameSystem"; }

		void OnRegister  (aether::World& world) override;
		void Update      (aether::World& world, float dt) override;
		void OnUnregister(aether::World& world) override;

		[[nodiscard]] int  GetActiveBodyCount()  const { return m_activeBodyCount; }
		[[nodiscard]] int  GetProjectileCount()  const { return m_projectileCount; }
		[[nodiscard]] float GetSimTime()         const { return m_simTime; }
		[[nodiscard]] aether::CameraHandle GetOrbitCamera() const { return m_orbitCamera; }
		[[nodiscard]] aether::CameraHandle GetFreeCamera()  const { return m_freeCamera; }

	private:
		void BuildScene(aether::World& world);
		void ClearScene(aether::World& world);
		void FireProjectile(aether::World& world);

		// Dependencies
		aether::AetherCore*    m_engine  = nullptr;
		aether::AssetManager*  m_assets  = nullptr;
		aether::CameraManager* m_cameras = nullptr;
		aether::Input*         m_input   = nullptr;
		aether::PhysicsSystem* m_physics = nullptr;

		// Cameras
		aether::CameraHandle m_orbitCamera;
		aether::CameraHandle m_freeCamera;

		// Rendering
		aether::GraphicsPipeline       m_pipeline;
		aether::Material               m_groundMaterial;
		aether::Material               m_boxMaterial;
		aether::Material               m_roundBodyMaterial;  // cubes used for round bodies
		aether::Material               m_projectileMaterial;
		aether::Material               m_wallMaterial;

		// Entity tracking for scene reset
		std::vector<aether::Entity>    m_sceneEntities;

		// Per-frame stats
		float m_simTime        = 0.0f;
		int   m_activeBodyCount = 0;
		int   m_projectileCount = 0;
		float m_projectileCooldown = 0.0f;

		std::mt19937 m_rng{ 1337 };

		static constexpr float  kGroundHalfExtent = 20.0f;
		static constexpr float  kGroundThickness  = 0.5f;
		static constexpr int    kStackWidth       = 4;
		static constexpr int    kStackHeight      = 6;
		static constexpr int    kScatterCount     = 20;
		static constexpr float  kProjectileRadius = 0.6f;
		static constexpr float  kProjectileSpeed  = 28.0f;
		static constexpr float  kProjectileCooldownTime = 0.3f;
	};

} // namespace aether::app
