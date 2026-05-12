#pragma once

#include <glm/glm.hpp>
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
	class AssetManager;
	class Input;
} // namespace aether

namespace aether::app
{
	class FishingGameSystem : public aether::System
	{
	public:
		void Init(ServiceContainer& services, aether::AssetManager& assets, aether::CameraManager& cameras, aether::Input& input);

		[[nodiscard]] const char* GetName() const override
		{
			return "FishingGameSystem";
		}

		void OnRegister(aether::World& world) override;
		void Update(aether::World& world, float dt) override;
		void OnUnregister(aether::World& world) override;

		[[nodiscard]] std::size_t GetFishCount() const
		{
			return m_fishAgents.size();
		}

		[[nodiscard]] std::size_t GetScore() const
		{
			return m_score;
		}

		[[nodiscard]] const char* GetBobberStateName() const;

		void UpdatePlayer(float dt);
		bool TryGetWaterHitPoint(glm::vec3& outTarget) const;
		glm::vec3 GetRodWorldOrigin() const;

		enum class BobberState
		{
			Ready,
			Casting,
			Waiting,
			Biting,
			Hooked,
		};

		static constexpr int kFishCount = 12;
		static constexpr float kLakeRadius = 22.0f;
		static constexpr float kWaterSize = 56.0f;
		static constexpr float kBobberHeight = 0.18f;
		static constexpr float kBobberScale = 0.22f;
		static constexpr float kFishScale = 0.34f;
		static constexpr float kMaxCastDistance = 32.0f;
		static constexpr float kCastSpeed = 45.0f;
		static constexpr float kCastArcBase = 1.8f;
		static constexpr float kCastArcScale = 1.4f;
		static constexpr float kFishSpeedMin = 2.0f;
		static constexpr float kFishSpeedMax = 4.5f;
		static constexpr float kLineThickness = 0.04f;
		static constexpr float kReelSpeed = 10.0f;
		static constexpr float kBiteHoldTime = 3.0f;
		static constexpr float kCatchRadius = 0.8f;

	private:
		struct FishingEntityTag
		{
			bool value = true;
		};

		struct FishAgent
		{
			glm::vec3 pos{ 0.0f };
			glm::vec3 target{ 0.0f };
			float heading = 0.0f;
			float speed = 0.0f;
			float bobPhase = 0.0f;
			bool hooked = false;
		};

		// Dependencies (set by Init)
		ServiceContainer* m_services = nullptr;
		aether::AssetManager* m_assets = nullptr;
		aether::CameraManager* m_cameras = nullptr;
		aether::Input* m_input = nullptr;

		struct PlayerController
		{
			glm::vec3 position{ 0.0f, 2.0f, -16.0f };
			float yaw = 180.0f;
			float pitch = -12.0f;
			float moveSpeed = 12.0f;
			float lookSpeed = 0.14f;
			float headHeight = 1.65f;
		};

		aether::CameraHandle m_playerCamera;
		PlayerController m_player;

		aether::GraphicsPipeline m_pipeline;
		aether::Material m_waterMaterial;
		aether::Material m_shoreMaterial;
		aether::Material m_fishMaterial;
		aether::Material m_bobberMaterial;
		const aether::Mesh* m_planeMesh = nullptr;
		const aether::Mesh* m_cubeMesh = nullptr;

		aether::Entity m_waterEntity;
		aether::Entity m_bobberEntity;
		aether::Entity m_lineEntity;
		std::vector<aether::Entity> m_fishEntities;

		std::vector<FishAgent> m_fishAgents;
		std::mt19937 m_rng{ 12345 };

		aether::Material m_lineMaterial;

		BobberState m_bobberState = BobberState::Ready;
		glm::vec3 m_bobberPos{ 0.0f };
		glm::vec3 m_bobberTarget{ 0.0f };
		glm::vec3 m_castOrigin{ 0.0f };
		glm::vec3 m_castDirection{ 0.0f, 0.0f, 1.0f };
		float m_castProgress = 0.0f;
		float m_castTotalDistance = 0.0f;
		float m_waitTimer = 0.0f;
		float m_biteTimer = 0.0f;
		int m_hookedFishIndex = -1;

		std::size_t m_score = 0;
		float m_time = 0.0f;
	};
} // namespace aether::app
