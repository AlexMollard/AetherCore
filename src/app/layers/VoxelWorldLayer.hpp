#pragma once

#include "AppLayer.hpp"
#include "camera/CameraManager.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "voxel/BlockRegistry.hpp"
#include "voxel/ChunkManager.hpp"

namespace aether::app
{
	class VoxelWorldLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		void GenerateTerrain();

		aether::GraphicsPipeline m_pipeline;
		voxel::BlockRegistry m_blockRegistry;
		voxel::ChunkManager m_chunkManager;

		aether::CameraHandle m_camera;
		bool m_prevForceVisible = false;
		bool m_prevBypassIndirect = false;
	};
} // namespace aether::app
