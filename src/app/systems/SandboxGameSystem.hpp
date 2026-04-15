#pragma once

#include "System.hpp"
#include "Entity.hpp"
#include "Camera.hpp"
#include "GraphicsPipeline.hpp"
#include "Material.hpp"
#include "CameraManager.hpp"
#include "AetherCore.hpp"

#include <optional>
#include <string_view>

namespace aether
{
	class Texture;
	class AssetManager;
	class Input;
}

namespace aether::app
{
	// Game logic system: manages all sandbox scene entities and their behavior.
	// This separates game logic from the UI layer, making inter-system communication
	// cleaner (AI system, movement system, etc. all operate on the world via systems).
	class SandboxGameSystem : public aether::System
	{
	public:
		// Initialize with engine dependencies. Must be called before registering.
		void Init(aether::AetherCore& engine,
		          aether::AssetManager& assets,
		          aether::CameraManager& cameras,
		          aether::Input& input);

		const char* GetName() const override { return "SandboxGameSystem"; }
		void OnRegister(aether::World& world) override;
		void Update(aether::World& world, float dt) override;
		void OnUnregister(aether::World& world) override;

		[[nodiscard]] float GetTimeSeconds() const { return m_time; }
		[[nodiscard]] std::size_t GetRingCount() const { return kRingCount; }
		[[nodiscard]] std::size_t GetModelEntityCount() const { return m_modelEntityCount; }
		[[nodiscard]] std::size_t GetModelPrimitiveCount() const { return m_model ? m_model->primitives.size() : 0; }
		[[nodiscard]] std::uint32_t GetAnimationCount() const;
		[[nodiscard]] std::string_view GetCurrentAnimationName() const;
		[[nodiscard]] aether::CameraHandle GetOrbitCameraHandle() const { return m_orbitCamera; }
		[[nodiscard]] aether::CameraHandle GetFreeCameraHandle() const { return m_freeCamera; }
		[[nodiscard]] aether::CameraHandle GetRttCameraHandle() const { return m_rttCamera; }

	private:
		// Dependencies (set by Init)
		aether::AetherCore* m_engine = nullptr;
		aether::AssetManager* m_assets = nullptr;
		aether::CameraManager* m_cameras = nullptr;
		aether::Input* m_input = nullptr;

		// Scene entities are tagged in the world; no handles stored here.
		std::size_t m_modelEntityCount = 0;

		// Cameras
		aether::CameraHandle m_orbitCamera;
		aether::CameraHandle m_freeCamera;
		aether::CameraHandle m_rttCamera;

		// Assets
		aether::GraphicsPipeline m_pipeline;
		aether::Texture m_debugTexture;
		aether::Material m_debugTexturedMaterial;
		aether::Material m_untexturedMaterial;
		aether::Material m_rttFeedMaterial;
		std::optional<aether::LoadedModel> m_model;
		const aether::Mesh* m_cubeMesh = nullptr;
		const aether::Mesh* m_quadMesh = nullptr;

		// Render target
		aether::AetherCore::CameraRenderTarget m_rttTarget;

		// Game time
		float m_time = 0.0f;

		static constexpr int kRingCount = 8;
	};
}
