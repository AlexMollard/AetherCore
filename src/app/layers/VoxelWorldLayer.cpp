#include "VoxelWorldLayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

#include "AetherCore.hpp"
#include "Camera.hpp"
#include "CameraManager.hpp"
#include "Logger.hpp"
#include "OverlayStyle.hpp"
#include "RenderQueue.hpp"
#include "UiLayout.hpp"
#include "UIRenderer.hpp"

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
		using namespace overlay;

		constexpr glm::vec2 kAnchor{ 0.f, 0.f };
		constexpr float kPanelL = 12.0f;
		constexpr float kPanelR = 430.0f;
		constexpr float kPanelTop = 12.0f;
		constexpr float kPanelBot = 408.0f;
		constexpr float kInnerL = kPanelL + kPad;
		constexpr float kInnerR = kPanelR - kPad;
		constexpr float kColKey = kInnerL;
		constexpr float kColVal = kInnerL + 190.0f;

		float SampleTerrain2D(int worldX, int worldZ)
		{
			const float x = static_cast<float>(worldX);
			const float z = static_cast<float>(worldZ);

			const float broad = std::sin(x * 0.035f) * static_cast<float>(kHillAmpA)
			                  + std::cos(z * 0.028f) * static_cast<float>(kHillAmpB);
			const float detail = std::sin((x + z) * 0.085f) * 3.5f
			                   + std::cos((x - z) * 0.072f) * 2.0f;
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

			return std::sin(x * 0.091f + y * 0.113f)
			     + std::cos(z * 0.087f - y * 0.097f)
			     + std::sin((x + z) * 0.043f + y * 0.067f)
			     + std::cos((x - z) * 0.052f - y * 0.041f);
		}

		bool IsSolidVoxel(int worldX, int worldY, int worldZ)
		{
			const float baseSurface = SampleTerrain2D(worldX, worldZ);
			float density = baseSurface - static_cast<float>(worldY);

			// Cliff shelf / overhang layer that protrudes beyond the base column.
			const float cliffMask = 0.5f + 0.5f * std::sin(static_cast<float>(worldX) * 0.024f + std::cos(static_cast<float>(worldZ) * 0.031f) * 1.7f);
			const float shelfCenter = baseSurface + 5.5f
			                        + std::sin(static_cast<float>(worldZ) * 0.079f) * 3.0f
			                        + std::cos(static_cast<float>(worldX) * 0.063f) * 2.0f;
			const float shelfThickness = 2.2f + 1.0f * std::sin(static_cast<float>(worldX + worldZ) * 0.05f);
			const float shelfDensity = (shelfThickness - std::abs(static_cast<float>(worldY) - shelfCenter)) * std::max(0.0f, cliffMask - 0.35f) * 2.4f;
			density = std::max(density, shelfDensity);

			// Carve cave pockets in the interior but leave enough roof/floor thickness.
			if (worldY > 6 && worldY < static_cast<int>(baseSurface) - 2)
			{
				if (CaveNoise3D(worldX, worldY, worldZ) > 2.15f)
					density -= 7.0f;
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
		INFO(LogCategory::App, "VoxelWorldLayer attached.");

		aether::AetherCore& engine = context.engine;
		m_prevForceVisible = engine.GetRenderQueue().IsDebugForceVisible();
		m_prevBypassIndirect = engine.GetRenderQueue().IsDebugBypassIndirect();
		engine.GetRenderQueue().SetDebugForceVisible(true);
		engine.GetRenderQueue().SetDebugBypassIndirect(false);

		// ── Pipeline ──────────────────────────────────────────────────────────
		const VkDescriptorSetLayout bindlessLayout = engine.GetBindlessManager().GetLayout();
		// Voxel shader only uses bindless set 0; no tiled-light set needed.
		const std::array<VkDescriptorSetLayout, 1> setLayouts{ bindlessLayout };

		m_pipeline = engine.CreateGraphicsPipeline({
		        .shaderVfsPath = "shaders://voxel_chunk.slang.spv",
		        .colorFormat = aether::AetherCore::GetForwardColorFormat(),
		        .depthFormat = engine.GetSwapchainDepthFormat(),
		        .depthTestEnable = true,
		        .depthWriteEnable = true,
		        .voxelVertexInput = true,
		        .setLayouts = std::span<const VkDescriptorSetLayout>(setLayouts.data(), setLayouts.size()),
		});

		// ── Block registry ────────────────────────────────────────────────────
		m_blockRegistry.Initialize(engine, kAtlasPath);

		m_blockRegistry.Register(voxel::BlockId::Stone, kUVStone);
		m_blockRegistry.Register(voxel::BlockId::Dirt, kUVDirt);
		m_blockRegistry.Register(voxel::BlockId::Grass,
		        /* top */ kUVGrassTop,
		        /* side */ kUVGrassSide,
		        /* bottom */ kUVDirt);

		// ── Chunk manager ─────────────────────────────────────────────────────
		m_chunkManager.Initialize(engine, m_blockRegistry, &m_pipeline);

		// ── Terrain ───────────────────────────────────────────────────────────
		GenerateTerrain();
		// Mesh uploads are throttled by ChunkManager::kMaxUploadsPerFrame.
		// The first few frames of OnUpdate will progressively upload all chunks.

		// ── Camera ────────────────────────────────────────────────────────────
		m_camera = context.cameras->Create({
		        .mode = aether::CameraMode::Free,
		        .position = { 0.0f, static_cast<float>(kBaseHeight) + 24.0f, 48.0f },
		        .yaw = 180.0f,
		        .pitch = -20.0f,
		        .moveSpeed = 20.0f,
		        .lookSpeed = 0.14f,
		});
		context.cameras->SetMainCamera(m_camera);

		// ── Lighting: bright directional, gentle ambient ───────────────────────
		engine.SetDirectionalLight(glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f)), 2.2f);
		engine.SetAmbientLight(glm::vec3(0.25f, 0.32f, 0.40f));
	}

	void VoxelWorldLayer::OnDetach(LayerContext& context)
	{
		context.engine.GetRenderQueue().SetDebugForceVisible(m_prevForceVisible);
		context.engine.GetRenderQueue().SetDebugBypassIndirect(m_prevBypassIndirect);
		m_chunkManager.Shutdown(context.engine);
		m_blockRegistry.Shutdown(context.engine);
		m_pipeline.Destroy();

		if (m_camera.IsValid())
			context.cameras->Destroy(m_camera);

		INFO(LogCategory::App, "VoxelWorldLayer detached.");
	}

	void VoxelWorldLayer::OnUpdate(LayerContext& context)
	{
		const aether::Camera* cam = context.cameras->TryGet(m_camera);
		const glm::vec3 playerPos = cam ? cam->GetPosition() : glm::vec3(0.0f);

		m_chunkManager.Update(playerPos);
		m_chunkManager.SubmitDraws(context.engine);
	}

	void VoxelWorldLayer::OnGui(LayerContext& context)
	{
		if (context.ui == nullptr)
			return;

		aether::UIRenderer& ui = *context.ui;
		std::array<char, 128> buf{};

		DrawPanel(ui, kAnchor, kPanelL, kPanelR, kPanelTop, kPanelBot);
		ui.DrawText("VOXEL WORLD",
		        aether::UiPoint{
		                .anchor = kAnchor, .offsetPx = { kInnerL, kPanelTop + 26.0f }
        },
		        18.0f,
		        kColorTitle);

		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kPanelTop + 54.0f);

		constexpr float kWorldY = kPanelTop + 68.0f;
		DrawSectionHeader(ui, "WORLDGEN", kAnchor, kPanelL, kInnerL, kWorldY);
		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kWorldY + 13.0f);

		float rowY = kWorldY + 34.0f;
		std::snprintf(buf.data(), buf.size(), "%d", kWorldRadius);
		DrawKV(ui, "Radius (chunks)", buf.data(), kAnchor, kColKey, kColVal, rowY);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%d", (kWorldRadius * 2 + 1) * (kWorldRadius * 2 + 1));
		DrawKV(ui, "Target XY chunks", buf.data(), kAnchor, kColKey, kColVal, rowY);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%d", voxel::kChunkSize);
		DrawKV(ui, "Chunk size", buf.data(), kAnchor, kColKey, kColVal, rowY);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%d", kTotalHeight);
		DrawKV(ui, "Max terrain Y", buf.data(), kAnchor, kColKey, kColVal, rowY);
		rowY += kRowH;

		const aether::Camera* cam = context.cameras ? context.cameras->TryGet(m_camera) : nullptr;
		if (cam)
		{
			const glm::vec3 p = cam->GetPosition();
			std::snprintf(buf.data(), buf.size(), "%.1f, %.1f, %.1f", p.x, p.y, p.z);
			DrawKV(ui, "Camera pos", buf.data(), kAnchor, kColKey, kColVal, rowY);
			rowY += kRowH;
		}

		constexpr float kChunkY = kPanelTop + 212.0f;
		DrawSectionHeader(ui, "CHUNK RUNTIME", kAnchor, kPanelL, kInnerL, kChunkY);
		DrawSeparator(ui, kAnchor, kInnerL, kInnerR, kChunkY + 13.0f);

		const voxel::ChunkManager::DebugStats stats = m_chunkManager.GetDebugStats();
		rowY = kChunkY + 34.0f;

		std::snprintf(buf.data(), buf.size(), "%zu", stats.totalChunks);
		DrawKV(ui, "Total chunks", buf.data(), kAnchor, kColKey, kColVal, rowY);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%zu", stats.readyChunks);
		DrawKV(ui, "Ready chunks", buf.data(), kAnchor, kColKey, kColVal, rowY, stats.readyChunks > 1 ? kColorGood : kColorWarn);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%zu", stats.dirtyChunks);
		DrawKV(ui, "Dirty chunks", buf.data(), kAnchor, kColKey, kColVal, rowY, stats.dirtyChunks == 0 ? kColorGood : kColorWarn);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%zu", stats.emptyChunks);
		DrawKV(ui, "Empty chunks", buf.data(), kAnchor, kColKey, kColVal, rowY);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%u", stats.submittedDrawsLastFrame);
		DrawKV(ui, "Submitted draws", buf.data(), kAnchor, kColKey, kColVal, rowY, stats.submittedDrawsLastFrame > 1 ? kColorGood : kColorWarn);
		rowY += kRowH;

		DrawKV(ui, "Draw path", context.engine.GetRenderQueue().IsDebugBypassIndirect() ? "Bypass indirect" : "Indirect", kAnchor, kColKey, kColVal, rowY, context.engine.GetRenderQueue().IsDebugBypassIndirect() ? kColorWarn : kColorGood);
		rowY += kRowH;

		std::snprintf(buf.data(), buf.size(), "%u / %u / %u", stats.rebuildAttemptsLastFrame, stats.rebuildUploadsLastFrame, stats.rebuildFailuresLastFrame);
		DrawKV(ui, "Rebuild A/U/F", buf.data(), kAnchor, kColKey, kColVal, rowY, stats.rebuildFailuresLastFrame == 0 ? kColorGood : kColorWarn);
	}

	// ── Terrain generation ────────────────────────────────────────────────────

	void VoxelWorldLayer::GenerateTerrain()
	{
		// Fill a 3D density field so caves and slight overhangs can appear.
		for (int cx = -kWorldRadius; cx <= kWorldRadius; ++cx)
			for (int cz = -kWorldRadius; cz <= kWorldRadius; ++cz)
			{
				for (int localZ = 0; localZ < voxel::kChunkSize; ++localZ)
					for (int localX = 0; localX < voxel::kChunkSize; ++localX)
					{
						const int worldX = cx * voxel::kChunkSize + localX;
						const int worldZ = cz * voxel::kChunkSize + localZ;
						const int surfaceY = SurfaceHeightAt(worldX, worldZ);

						for (int worldY = 0; worldY < kTotalHeight; ++worldY)
						{
							if (!IsSolidVoxel(worldX, worldY, worldZ))
								continue;

							voxel::BlockId id;
							if (worldY >= surfaceY)
								id = voxel::BlockId::Grass;
							else if (worldY >= surfaceY - (kDirtDepth - 1))
								id = voxel::BlockId::Dirt;
							else
								id = voxel::BlockId::Stone;

							m_chunkManager.SetBlock({ worldX, worldY, worldZ }, id);
						}
					}
			}
	}
} // namespace aether::app
