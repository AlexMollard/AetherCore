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
		std::uint32_t nodePoseOffset = 0;
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

	// ── Animation blend ─────────────────────────────────────────────────────

	// Drives cross-fade blending between two animation clips.
	// primaryClip is the currently playing clip (managed by SkinnedMeshComponent).
	// When a transition is requested via set_animation_blend(), secondaryClip
	// and transitionSpeed are set, and inTransition=true. The AnimationBlendSystem
	// advances blendWeight toward 0 each frame; when it reaches 0 the transition
	// completes and primaryClip is replaced by secondaryClip.
	struct AnimationBlendComponent
	{
		std::uint32_t primaryClip = 0;          // currently active clip index in animDb
		std::uint32_t secondaryClip = 0;        // clip to transition to (0 = no transition)
		float blendWeight = 1.0f;              // 1.0 = fully primary, 0.0 = fully secondary
		float transitionSpeed = 4.0f;           // blend weight reduction per second
		bool inTransition = false;
	};

	// ── IK targets ─────────────────────────────────────────────────────────

	// Per-entity IK state for foot-planting and ground snapping.
	// Populated once at spawn from bone name lookups, then updated per-frame
	// by the IK system with raycast results and computed adjustments.
	struct IkTargetsComponent
	{
		// Node indices for each leg chain (set once at spawn).
		std::uint32_t hipsNodeIdx = UINT32_MAX;
		std::uint32_t leftKneeNodeIdx = UINT32_MAX;
		std::uint32_t leftFootNodeIdx = UINT32_MAX;
		std::uint32_t rightKneeNodeIdx = UINT32_MAX;
		std::uint32_t rightFootNodeIdx = UINT32_MAX;

		// Bind-pose leg lengths (set once at spawn from skeleton data).
		float leftUpperLegLen = 0.0f;
		float leftLowerLegLen = 0.0f;
		float rightUpperLegLen = 0.0f;
		float rightLowerLegLen = 0.0f;

		// Knee bend direction: +1 or -1 (computed from bind-pose knee vs hip-to-foot direction).
		// Used by the two-bone IK solver to determine which side the knee bends.
		float leftKneeBendSign = 1.0f;
		float rightKneeBendSign = 1.0f;

		// Per-frame ground detection results (written by CPU raycast, read by GPU IK pass).
		bool leftFootPlanted = false;
		bool rightFootPlanted = false;
		float leftFootGroundY = 0.0f;
		float rightFootGroundY = 0.0f;
		float raycastMaxDist = 2.0f; // maximum downward raycast distance for foot grounding

		// IK correction offsets applied to each foot in world space.
		glm::vec3 leftFootOffset{0.0f, 0.0f, 0.0f};
		glm::vec3 rightFootOffset{0.0f, 0.0f, 0.0f};

		// Global IK enable/disable (cheap toggle without removing component).
		bool enabled = true;
	};

	// ── Root motion ─────────────────────────────────────────────────────────

	// Tracks root bone motion state for physics-driven character movement.
	// Each frame the AnimationRootMotionSystem reads the Hips node's world position
	// from the node global transforms buffer, computes the delta since the previous
	// frame, and applies that delta to the entity's PhysicsStateComponent so the
	// character physically moves through the world rather than sliding in place.
	struct RootMotionComponent
	{
		std::uint32_t hipsNodeIdx = UINT32_MAX;  // node index of the root bone (e.g. Hips)
		glm::vec3 prevHipsWorldPos{0.0f, 0.0f, 0.0f};   // world position last frame
		glm::vec3 accumulatedDelta{0.0f, 0.0f, 0.0f};  // total delta since animation started
		bool applyToPhysics = true;   // push delta to PhysicsStateComponent (physics-driven movement)
		bool applyToTransform = true; // push delta to TransformComponent (visual sync)
		bool enabled = true;
	};
} // namespace aether
