#pragma once

#include <array>

#include "AppLayer.hpp"
#include "CameraManager.hpp"
#include "Entity.hpp"
#include "GraphicsPipeline.hpp"
#include "AetherCore.hpp"
#include "Texture.hpp"

namespace aether
{
	class Mesh;
}

namespace aether::app
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

		aether::GraphicsPipeline m_pipeline;
		const aether::Mesh* m_cubeMesh = nullptr;
		const aether::Mesh* m_quadMesh = nullptr;
		aether::Texture          m_debugTexture;

		aether::Entity m_groundEntity;
		aether::Entity m_centerEntity;
		std::array<aether::Entity, kRingCount> m_ringEntities{};
		aether::Entity m_orbitEntityA;
		aether::Entity m_orbitEntityB;
		aether::CameraHandle m_orbitCamera;
		aether::CameraHandle m_freeCamera;
		aether::CameraHandle m_rttCamera;
		aether::AetherCore::CameraRenderTarget m_rttTarget;

		float m_time = 0.0f;
		float m_cameraAngle = 0.0f;
	};
}