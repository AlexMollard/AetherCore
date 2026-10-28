#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <vector>
#include <glm/glm.hpp>
#include "mesh/Mesh.hpp"
#include <array>
#include "mesh/GrassMeshData.hpp"
#include "material/MaterialHandle.hpp"
#include "material/MaterialAsset.hpp"

namespace aether
{
	class GraphicsPipeline;
	struct GrassRuntimeChunk
	{
		glm::vec3 origin{0.0f};
		Mesh mesh;
		MeshGen::GrassChunkKey key{};
		float worldDisplacement = 0.0f;
		std::atomic<bool> ready{false};
		bool failed = false;
	};

	struct GrassUpload
	{
		std::shared_ptr<GrassRuntimeChunk> chunk;
		MeshGen::MeshData mesh;
	};

	struct GrassMaterialState
	{
		std::array<MaterialHandle, 3> handles{};
		std::array<std::uint32_t, 3> slots{};
		const GraphicsPipeline* pipeline = nullptr;
		std::atomic<bool> ready{false};
	};

	struct GrassMaterialRequest
	{
		std::shared_ptr<GrassMaterialState> state;
		MaterialAsset material;
		MaterialHandle sourceKeepalive{};
		std::array<glm::vec2, 3> fades;
	};

	struct GrassStreamingCommands
	{
		std::vector<GrassUpload> uploads;
		std::vector<GrassMaterialRequest> materials;
		std::vector<std::shared_ptr<GrassMaterialState>> retiredMaterials;
		std::vector<std::shared_ptr<GrassRuntimeChunk>> retired;
	};

	struct GrassRuntimeDraw
	{
		const GrassRuntimeChunk* chunk = nullptr;
		std::uint32_t materialSlot = 0xFFFFFFFFu;
	};

	// References remain valid until the renderer is drained.
	struct GrassRuntimeComponent
	{
		std::uint64_t key = 0;
		const GraphicsPipeline* pipeline = nullptr;
		std::vector<MeshGen::GrassChunkKey> selected;
		std::vector<GrassRuntimeDraw> draws;
	};
}
