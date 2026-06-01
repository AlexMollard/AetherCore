#include "VoxelWorldLayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

#include "ui/UIRenderer.hpp"
#include "ui/UiLayout.hpp"

#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "passes/PostProcessStack.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderQueue.hpp"
#include "assets/AssetManager.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
	// ── Terrain generation constants ──────────────────────────────────────────

	// More vertical range plus a slightly smaller footprint keeps the terrain
	// interesting without exploding initial chunk cost.
	static constexpr int kWorldRadius = 4;  // chunks in each direction on X/Z
	static constexpr int kGroundChunks = 2; // more vertical range for caves / overhangs

	static constexpr int kTotalHeight = voxel::kChunkSize * kGroundChunks;
	static constexpr int kBaseHeight = 18;
	static constexpr int kHillAmpA = 10;
	static constexpr int kHillAmpB = 6;
	static constexpr int kDirtDepth = 4;

	namespace
	{
		UiRect PxRect(float l, float t, float r, float b)
		{
			return UiRect{
				.anchorMin = { 0.f, 0.f },
				.anchorMax = { 0.f, 0.f },
				.offsetMinPx = { l, t },
				.offsetMaxPx = { r, b }
			};
		}

		float SampleTerrain2D(int worldX, int worldZ)
		{
			const float x = static_cast<float>(worldX);
			const float z = static_cast<float>(worldZ);

			const float broad = std::sin(x * 0.035f) * static_cast<float>(kHillAmpA) + std::cos(z * 0.028f) * static_cast<float>(kHillAmpB);
			const float detail = std::sin((x + z) * 0.085f) * 3.5f + std::cos((x - z) * 0.072f) * 2.0f;
			const float ridges = std::abs(std::sin(x * 0.018f) + std::cos(z * 0.022f)) * 5.5f;

			return static_cast<float>(kBaseHeight) + broad + detail + ridges;
		}

		int SurfaceHeightAt(int worldX, int worldZ)
		{
			const int surface = static_cast<int>(std::round(SampleTerrain2D(worldX, worldZ)));
			return std::clamp(surface, 8, kTotalHeight - 10);
		}

		float CaveNoise3D(int worldX, int worldY, int worldZ)
		{
			const float x = static_cast<float>(worldX);
			const float y = static_cast<float>(worldY);
			const float z = static_cast<float>(worldZ);

			return std::sin(x * 0.091f + y * 0.113f) + std::cos(z * 0.087f - y * 0.097f) + std::sin((x + z) * 0.043f + y * 0.067f) + std::cos((x - z) * 0.052f - y * 0.041f);
		}

		bool IsSolidVoxel(int worldX, int worldY, int worldZ)
		{
			const float baseSurface = SampleTerrain2D(worldX, worldZ);
			float density = baseSurface - static_cast<float>(worldY);

			// Cliff shelf / overhang layer that protrudes beyond the base column.
			const float cliffMask = 0.5f + 0.5f * std::sin(static_cast<float>(worldX) * 0.024f + std::cos(static_cast<float>(worldZ) * 0.031f) * 1.7f);
			const float shelfCenter = baseSurface + 5.5f + std::sin(static_cast<float>(worldZ) * 0.079f) * 3.0f + std::cos(static_cast<float>(worldX) * 0.063f) * 2.0f;
			const float shelfThickness = 2.2f + 1.0f * std::sin(static_cast<float>(worldX + worldZ) * 0.05f);
			const float shelfDensity = (shelfThickness - std::abs(static_cast<float>(worldY) - shelfCenter)) * std::max(0.0f, cliffMask - 0.35f) * 2.4f;
			density = std::max(density, shelfDensity);

			// Carve cave pockets in the interior but leave enough roof/floor thickness.
			if (worldY > 6 && worldY < static_cast<int>(baseSurface) - 2)
			{
				if (CaveNoise3D(worldX, worldY, worldZ) > 2.15f)
				{
					density -= 7.0f;
				}
			}

			return density > 0.0f;
		}
	} // namespace

	// ── UV helpers ────────────────────────────────────────────────────────────
	//
	// Atlas layout is an evenly sliced N×N grid.
	// Change kAtlasTilesPerAxis to switch from 4x4 to 8x8, etc.
	// Row 0 (top): Grass-side(0,0) | Grass-top(1,0) | Dirt(2,0) | Stone(3,0)
	// (Exact UVs are tuned to match whatever atlas you place at kAtlasPath.)

	static constexpr int kAtlasTilesPerAxis = 4;
	static constexpr int kAtlasPixelsPerTile = 16;
	static constexpr float kAtlasInsetTexels = 1.0f;
	static constexpr float kTile = 1.0f / static_cast<float>(kAtlasTilesPerAxis);
	static constexpr float kAtlasTexel = 1.0f / static_cast<float>(kAtlasTilesPerAxis * kAtlasPixelsPerTile);

	static constexpr voxel::FaceUV UV(int col, int row)
	{
		const float inset = kAtlasInsetTexels * kAtlasTexel;
		return {
			.uvMin = {       col * kTile + inset,       row * kTile + inset },
              .uvMax = { (col + 1) * kTile - inset, (row + 1) * kTile - inset }
		};
	}

	static constexpr voxel::FaceUV kUVGrassTop = UV(1, 0);
	static constexpr voxel::FaceUV kUVGrassSide = UV(0, 0);
	static constexpr voxel::FaceUV kUVDirt = UV(2, 0);
	static constexpr voxel::FaceUV kUVStone = UV(3, 0);

	// VFS path to the block atlas texture (bundled as a fallback white 1×1 if missing).
	static constexpr std::string_view kAtlasPath = "assets://textures/blocks/atlas.png";

	// ── Layer lifecycle ───────────────────────────────────────────────────────

	void VoxelWorldLayer::OnAttach(LayerContext& context)
	{
		AE_INFO(LogCategory::App, "VoxelWorldLayer attached.");

		ServiceContainer& s = context.services;

		m_prevForceVisible = s.Get<RenderQueue>().IsDebugForceVisible();
		m_prevBypassIndirect = s.Get<RenderQueue>().IsDebugBypassIndirect();
		s.Get<RenderQueue>().SetDebugForceVisible(true);
		s.Get<RenderQueue>().SetDebugBypassIndirect(false);

		// ── Pipeline ──────────────────────────────────────────────────────────
		const VkDescriptorSetLayout bindlessLayout = s.Get<BindlessManager>().GetLayout();
		const std::array<VkDescriptorSetLayout, 1> setLayouts{ bindlessLayout };

		AE_EXPECT_OR_THROW(pipeline,
		        s.Get<AssetManager>().CreateGraphicsPipeline({
		                .shaderVfsPath = "shaders://voxel_chunk.slang.spv",
		                .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		                .depthFormat = s.Get<Swapchain>().GetDepthFormat(),
		                .depthTestEnable = true,
		                .depthWriteEnable = true,
		                .setLayouts = std::span<const VkDescriptorSetLayout>(setLayouts.data(), setLayouts.size()),
		        }));
		m_pipeline = std::move(pipeline);

		// ── Block registry ────────────────────────────────────────────────────
		m_blockRegistry.Initialize(s, kAtlasPath);

		m_blockRegistry.Register(voxel::BlockId::Stone, kUVStone);
		m_blockRegistry.Register(voxel::BlockId::Dirt, kUVDirt);
		m_blockRegistry.Register(voxel::BlockId::Grass,
		        /* top */ kUVGrassTop,
		        /* side */ kUVGrassSide,
		        /* bottom */ kUVDirt);

		// ── Chunk manager ─────────────────────────────────────────────────────
		m_chunkManager.Initialize(s, m_blockRegistry, &m_pipeline);

		// ── Terrain ───────────────────────────────────────────────────────────
		GenerateTerrain();

		// ── Camera ────────────────────────────────────────────────────────────
		m_camera = s.Get<CameraManager>().Create({
		        .mode = aether::CameraMode::Free,
		        .position = { 0.0f, static_cast<float>(kBaseHeight) + 24.0f, 48.0f },
		        .yaw = 180.0f,
		        .pitch = -20.0f,
		        .moveSpeed = 20.0f,
		        .lookSpeed = 0.14f,
		});
		s.Get<CameraManager>().SetMainCamera(m_camera);

		// ── Lighting: bright directional, gentle ambient ───────────────────────
		s.Get<Renderer>().SetDirectionalLight(glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f)), 2.2f);
		s.Get<Renderer>().SetAmbientLight(glm::vec3(0.25f, 0.32f, 0.40f));
	}

	void VoxelWorldLayer::OnDetach(LayerContext& context)
	{
		ServiceContainer& s = context.services;
		s.Get<RenderQueue>().SetDebugForceVisible(m_prevForceVisible);
		s.Get<RenderQueue>().SetDebugBypassIndirect(m_prevBypassIndirect);
		m_chunkManager.Shutdown(s);
		m_blockRegistry.Shutdown(s);
		m_pipeline.Destroy();

		if (m_camera.IsValid())
		{
			s.Get<CameraManager>().Destroy(m_camera);
		}

		AE_INFO(LogCategory::App, "VoxelWorldLayer detached.");
	}

	void VoxelWorldLayer::OnUpdate(LayerContext& context)
	{
		const aether::Camera* cam = context.Get<CameraManager>().TryGet(m_camera);
		const glm::vec3 playerPos = cam ? cam->GetPosition() : glm::vec3(0.0f);

		m_chunkManager.Update(playerPos);
		m_chunkManager.SubmitDraws(context.services);
	}

	void VoxelWorldLayer::OnGui(LayerContext& context)
	{
		UIRenderer& ui = context.Get<UIRenderer>();

		constexpr float kPanelW = 320.f;
		constexpr float kPadX = 14.f;
		constexpr float kPadY = 10.f;
		constexpr float kRowH = 18.f;
		constexpr float kSepH = 16.f;

		const glm::vec4 bg{ 0.08f, 0.08f, 0.11f, 0.92f };
		const glm::vec4 white{ 0.93f, 0.93f, 0.93f, 1.f };
		const glm::vec4 green{ 0.40f, 0.72f, 0.46f, 1.f };
		const glm::vec4 yellow{ 0.86f, 0.71f, 0.30f, 1.f };
		const glm::vec4 red{ 0.80f, 0.33f, 0.30f, 1.f };

		const voxel::ChunkManager::DebugStats stats = m_chunkManager.GetDebugStats();
		const aether::Camera* cam = context.Get<CameraManager>().TryGet(m_camera);

		// Calculate panel height
		float contentH = kPadY;
		contentH += kRowH; // Radius
		contentH += kRowH; // Target XY chunks
		contentH += kRowH; // Chunk size
		contentH += kRowH; // Max terrain Y
		if (cam)
			contentH += kRowH; // Camera pos
		contentH += kSepH;
		contentH += kRowH; // Total chunks
		contentH += kRowH; // Ready chunks
		contentH += kRowH; // Dirty chunks
		contentH += kRowH; // Empty chunks
		contentH += kRowH; // Submitted draws
		contentH += kRowH; // Draw path
		contentH += kRowH; // Rebuild A/U/F
		contentH += kPadY;

		ui.DrawRect(PxRect(12.f, 12.f, 12.f + kPanelW, 12.f + contentH), bg, 6.f);

		float y = 12.f + kPadY;
		const float col2X = 12.f + 190.f;
		const float textSize = 13.f;

		std::array<char, 128> buf{};

		auto label = [&](const char* name, const char* value, glm::vec4 valueColor)
		{
			ui.DrawText(name, { .anchor = { 0.f, 0.f }, .offsetPx = { 12.f + kPadX, y } }, textSize, white);
			ui.DrawText(value, { .anchor = { 0.f, 0.f }, .offsetPx = { col2X, y } }, textSize, valueColor);
			y += kRowH;
		};

		std::snprintf(buf.data(), buf.size(), "%d", kWorldRadius);
		label("Radius (chunks)", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%d", (kWorldRadius * 2 + 1) * (kWorldRadius * 2 + 1));
		label("Target XY chunks", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%d", voxel::kChunkSize);
		label("Chunk size", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%d", kTotalHeight);
		label("Max terrain Y", buf.data(), white);

		if (cam)
		{
			const glm::vec3 p = cam->GetPosition();
			std::snprintf(buf.data(), buf.size(), "%.1f, %.1f, %.1f", p.x, p.y, p.z);
			label("Camera pos", buf.data(), white);
		}

		y += 4.f;

		std::snprintf(buf.data(), buf.size(), "%zu", stats.totalChunks);
		label("Total chunks", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%zu", stats.readyChunks);
		label("Ready chunks", buf.data(), stats.readyChunks > 1 ? green : yellow);

		std::snprintf(buf.data(), buf.size(), "%zu", stats.dirtyChunks);
		label("Dirty chunks", buf.data(), stats.dirtyChunks == 0 ? green : yellow);

		std::snprintf(buf.data(), buf.size(), "%zu", stats.emptyChunks);
		label("Empty chunks", buf.data(), white);

		std::snprintf(buf.data(), buf.size(), "%u", stats.submittedDrawsLastFrame);
		label("Submitted draws", buf.data(), stats.submittedDrawsLastFrame > 1 ? green : yellow);

		const bool bypass = context.Get<RenderQueue>().IsDebugBypassIndirect();
		label("Draw path", bypass ? "Bypass indirect" : "Indirect", bypass ? yellow : green);

		std::snprintf(buf.data(), buf.size(), "%u / %u / %u", stats.rebuildAttemptsLastFrame, stats.rebuildUploadsLastFrame, stats.rebuildFailuresLastFrame);
		label("Rebuild A/U/F", buf.data(), stats.rebuildFailuresLastFrame == 0 ? green : red);
	}

	// ── Terrain generation ────────────────────────────────────────────────────

	void VoxelWorldLayer::GenerateTerrain()
	{
		// Fill a 3D density field so caves and slight overhangs can appear.
		for (int cx = -kWorldRadius; cx <= kWorldRadius; ++cx)
		{
			for (int cz = -kWorldRadius; cz <= kWorldRadius; ++cz)
			{
				for (int localZ = 0; localZ < voxel::kChunkSize; ++localZ)
				{
					for (int localX = 0; localX < voxel::kChunkSize; ++localX)
					{
						const int worldX = cx * voxel::kChunkSize + localX;
						const int worldZ = cz * voxel::kChunkSize + localZ;
						const int surfaceY = SurfaceHeightAt(worldX, worldZ);

						for (int worldY = 0; worldY < kTotalHeight; ++worldY)
						{
							if (!IsSolidVoxel(worldX, worldY, worldZ))
							{
								continue;
							}

							voxel::BlockId id;
							if (worldY >= surfaceY)
							{
								id = voxel::BlockId::Grass;
							}
							else if (worldY >= surfaceY - (kDirtDepth - 1))
							{
								id = voxel::BlockId::Dirt;
							}
							else
							{
								id = voxel::BlockId::Stone;
							}

							m_chunkManager.SetBlock({ worldX, worldY, worldZ }, id);
						}
					}
				}
			}
		}
	}
} // namespace aether::app
