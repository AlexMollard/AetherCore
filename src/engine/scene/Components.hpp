#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "vulkan/volk.hpp"

#include "animation/AnimationDatabase.hpp"
#include "assets/GltfAsset.hpp"
#include "material/Material.hpp"

namespace aether
{
	class GraphicsPipeline;
	class Mesh;

	// ── ECS component types ────────────────────────────────────────────────
	// Each is a plain-data struct; the World owns all component storage.

	// World-space transform (model matrix).
	struct TransformComponent
	{
		glm::mat4 localToWorld{1.0f};
	};

	// Reference to a GPU vertex/index buffer.
	struct MeshComponent
	{
		const Mesh* mesh = nullptr;
	};

	// Surface shading properties (textures, tint, …).
	struct MaterialComponent
	{
		Material material{};
	};

	// Pipeline (shader + raster state) used to draw the entity.
	struct PipelineComponent
	{
		const GraphicsPipeline* pipeline = nullptr;
	};

	// Drives GPU-based skeletal animation for a skinned mesh entity.
	struct SkinnedMeshComponent
	{
		AnimationDatabase* animDb = nullptr;
		std::uint32_t skinIndex = 0;
		std::uint32_t jointCount = 0; // cached from animDb at spawn time
		std::uint32_t clipIndex = 0;
		float animTime = 0.f;
		float playbackSpeed = 1.f;
		bool looping = true;
		// Runtime-loaded animation clips, kept on CPU for duration queries.
		// Cleared after compile_animations() bakes them into animDb.
		std::vector<assets::GltfAnimation> pendingExternalAnims;
	};

	// Links a spawned mesh entity back to its parent script entity.
	struct ParentEntityComponent
	{
		std::uint32_t parentId = 0;
	};

	// Stores spawned mesh entity IDs on the script entity.
	struct SpawnedEntitiesComponent
	{
		std::vector<std::uint32_t> entityIds;
	};
} // namespace aether
