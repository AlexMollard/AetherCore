#pragma once

#include <array>

#include "AppLayer.hpp"
#include "CameraManager.hpp"
#include "Entity.hpp"
#include "GraphicsPipeline.hpp"
#include "MeowCore.hpp"
#include "Texture.hpp"

namespace meow
{
	class Mesh;
}

namespace meow::app
{
	class SandboxLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		static constexpr int kRingCount = 8;

		meow::GraphicsPipeline m_pipeline;
		const meow::Mesh* m_cubeMesh = nullptr;
		const meow::Mesh* m_quadMesh = nullptr;
		meow::Texture          m_debugTexture;

		meow::Entity m_groundEntity;
		meow::Entity m_centerEntity;
		std::array<meow::Entity, kRingCount> m_ringEntities{};
		meow::Entity m_orbitEntityA;
		meow::Entity m_orbitEntityB;
		meow::CameraHandle m_orbitCamera;
		meow::CameraHandle m_freeCamera;
		meow::CameraHandle m_rttCamera;
		meow::MeowCore::CameraRenderTarget m_rttTarget;

		float m_time = 0.0f;
		float m_cameraAngle = 0.0f;
	};
}