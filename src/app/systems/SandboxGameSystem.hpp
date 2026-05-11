#pragma once

#include <glm/glm.hpp>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

#include "scene/AetherCore.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "scene/Entity.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "material/Material.hpp"
#include "animation/ModelAnimator.hpp"
#include "scene/System.hpp"

namespace aether
{
	class Texture;
	class AssetManager;
	class Input;
} // namespace aether

namespace aether::app
{
	class SandboxGameSystem : public aether::System
	{
	public:
		void Init(aether::AetherCore& engine, aether::AssetManager& assets, aether::CameraManager& cameras, aether::Input& input);

		const char* GetName() const override
		{
			return "SandboxGameSystem";
		}

		void OnRegister(aether::World& world) override;
		void Update(aether::World& world, float dt) override;
		void OnUnregister(aether::World& world) override;

		[[nodiscard]] float GetTimeSeconds() const
		{
			return m_time;
		}

		[[nodiscard]] std::size_t GetRingCount() const
		{
			return kRingCount;
		}

		[[nodiscard]] std::size_t GetFoxCount() const
		{
			return kFoxCount;
		}

		[[nodiscard]] std::size_t GetFoxPrimitiveCount() const
		{
			return m_foxModel ? m_foxModel->primitives.size() : 0;
		}

		[[nodiscard]] std::uint32_t GetAnimationCount() const;
		[[nodiscard]] std::string_view GetCurrentAnimationName() const;

		[[nodiscard]] aether::CameraHandle GetOrbitCameraHandle() const
		{
			return m_orbitCamera;
		}

		[[nodiscard]] aether::CameraHandle GetFreeCameraHandle() const
		{
			return m_freeCamera;
		}

		[[nodiscard]] aether::CameraHandle GetRttCameraHandle() const
		{
			return m_rttCamera;
		}

	private:
		// Per-fox autonomous agent state.
		struct FoxAgent
		{
			glm::vec3 pos{ 0.0f };
			float heading = 0.0f; // radians, Y-axis rotation
			glm::vec3 target{ 0.0f };
			float stateTimer = 0.0f;
			bool idle = false;
		};

		// Dependencies (set by Init)
		aether::AetherCore* m_engine = nullptr;
		aether::AssetManager* m_assets = nullptr;
		aether::CameraManager* m_cameras = nullptr;
		aether::Input* m_input = nullptr;

		// Per-fox state and entity handles (parallel arrays, indexed by fox index)
		std::vector<FoxAgent> m_foxAgents;
		std::vector<std::vector<aether::Entity>> m_foxInstances;
		std::vector<aether::ModelAnimator> m_foxAnimators;
		std::mt19937 m_rng{ 42 };

		// Cameras
		aether::CameraHandle m_orbitCamera;
		aether::CameraHandle m_freeCamera;
		aether::CameraHandle m_rttCamera;

		// Assets
		aether::GraphicsPipeline m_pipeline;
		std::vector<aether::Texture> m_debugMaterialTextures;
		aether::Material m_debugTexturedMaterial;
		aether::Material m_untexturedMaterial;
		aether::Material m_rttFeedMaterial;
		std::vector<aether::Material> m_pointLightMarkerMaterials;
		std::vector<aether::Renderer::PointLight> m_pointLights;
		std::optional<aether::LoadedModel> m_foxModel;
		const aether::Mesh* m_cubeMesh = nullptr;
		const aether::Mesh* m_quadMesh = nullptr;
		const aether::Mesh* m_planeMesh = nullptr;

		// Render target
		aether::AetherCore::CameraRenderTarget m_rttTarget;

		// Game time
		float m_time = 0.0f;

		static constexpr int kRingCount = 8;
		static constexpr int kFoxCount = 30;
		static constexpr float kGroundHalfExtent = 80.0f;
		static constexpr float kFoxRunSpeed = 4.5f;
		// Animation playback speed for the run cycle.
		// Tune this so the leg motion matches the ground speed visually.
		static constexpr float kFoxAnimRunSpeed = 0.7f;
		static constexpr float kSkyHeight = 12.0f;
		static constexpr std::uint32_t kAnimSurvey = 0;
		static constexpr std::uint32_t kAnimRun = 2;
	};
} // namespace aether::app
