#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace aether
{
	enum class SpriteInstanceFlags : std::uint32_t
	{
		None = 0,
		FlipX = 1u << 0u,
		FlipY = 1u << 1u,
		PixelSnap = 1u << 2u,
		Masked = 1u << 3u,
		// Pixel-art sampling: the shader snaps UVs to texel centres, emulating
		// a nearest-neighbour sampler (set from the atlas' filter metadata).
		NearestFilter = 1u << 4u,
	};

	struct SpriteRenderInstance
	{
		glm::mat4 world{1.0f};
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f};
		glm::vec4 color{1.0f};
		glm::vec4 sizeAndPivot{1.0f, 1.0f, 0.5f, 0.5f};
		std::uint64_t sortKey = 0;
		std::uint32_t textureIndex = 0;
		std::uint32_t entityId = 0;
		SpriteInstanceFlags flags = SpriteInstanceFlags::None;
		std::uint32_t blendMode = 0;
	};

	// One 3D billboard particle: a camera-facing quad at a world position, sampled from a sub-rect
	// of a texture page. Sorted back-to-front against the camera at frame prep.
	// Mirrors GpuBillboardParticle in shaders/include/ParticleBillboard.slangh (64 bytes).
	struct BillboardParticleInstance
	{
		glm::vec4 positionSize{0.0f}; // xyz = world centre, w = quad edge (world units)
		glm::vec4 color{1.0f};
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f}; // (u0, v0, u1, v1), v down
		float rotationDegrees = 0.0f;             // clockwise on screen
		std::uint32_t textureIndex = 0;
		std::uint32_t blendMode = 0; // bit 0: additive (else alpha); kBillboardDisplaySpace: composite after the tonemap
		std::uint32_t entityId = 0;
	};

	// BillboardParticleInstance::blendMode bits.
	inline constexpr std::uint32_t kBillboardAdditive = 1u << 0u;
	inline constexpr std::uint32_t kBillboardDisplaySpace = 1u << 8u;
	inline constexpr std::uint32_t kBillboardLit = 1u << 9u; // shade as a sun-lit ball (ParticleEmitterComponent::lit)

	// Occluder2D::flags bits.
	inline constexpr std::uint32_t kOccluder2DFlipX = 1u << 0u;
	inline constexpr std::uint32_t kOccluder2DFlipY = 1u << 1u;
	// Capsule form (scripts): posHalfSize = (a.xy, radius), uvRect.xy = b; no texture is sampled.
	inline constexpr std::uint32_t kOccluder2DCapsule = 1u << 2u;

	// One shadow caster. Tiles use the quad form: the pass draws the cell and samples the tile's own
	// texture, so occlusion follows the ARTWORK's alpha (sub-tile) rather than the whole cell. Scripts
	// use the capsule form for transient strokes/trails.
	// Mirrors GpuOccluder2D in shaders/occluder2d.slang (48 bytes).
	struct Occluder2D
	{
		glm::vec4 posHalfSize{0.0f};              // quad: xy = world centre, z = half cell | capsule: xy = a, z = radius
		glm::vec4 uvRect{0.0f, 0.0f, 1.0f, 1.0f}; // quad: atlas region (image space) | capsule: xy = b
		std::uint32_t textureIndex = 0;
		std::uint32_t flags = 0; // see kOccluder2D* above
		std::uint32_t pad0 = 0;
		std::uint32_t pad1 = 0;
	};

	// Owned by RenderFramePacket and moved through the render-thread queue with
	// it. The game thread finishes extraction before submission; the render
	// thread may retain references only for the synchronous execution of that
	// packet. It must never reach back into the ECS or asset authoring objects.
	struct Render2DFrameData
	{
		// Sprites AND tile instances: tile extraction appends here so one sort
		// interleaves both by sort key. Finalize2DFrame must run after the last
		// producer and before submission.
		std::vector<SpriteRenderInstance> sprites;

		// Shadow occluders for 2D lighting: one entry per SOLID tile cell. The Light2D pass rasterises
		// these into an occluder mask (sampling each tile's texture, so occlusion is sub-tile) and
		// ray-marches it for shadows. Backgrounds and sprites do not occlude.
		std::vector<Occluder2D> occluders;

		// 3D billboard particles (ParticleEmitterComponent in Billboard3D space), drawn by
		// BillboardParticleRenderer in the 3D scene, depth-tested against the scene depth.
		std::vector<BillboardParticleInstance> billboards;
	};

	// Deterministic sort of the merged 2D instance stream (stable, by sortKey).
	void Finalize2DFrame(Render2DFrameData& frame);

} // namespace aether
