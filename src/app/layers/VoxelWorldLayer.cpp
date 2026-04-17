#include "VoxelWorldLayer.hpp"

#include <algorithm>
#include <array>
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
	} // namespace

	// ── Terrain generation constants ──────────────────────────────────────────

	// Small rolling terrain for the voxel showcase.
	// World extends ±kWorldRadius chunks on X/Z and is meshed as a heightfield,
	// not a fully solid prism, so the result reads as terrain instead of a box.
	static constexpr int kWorldRadius = 6;  // chunks in each direction on X/Z
	static constexpr int kGroundChunks = 1; // max terrain height fits in one chunk

	// Layer heights within the terrain (in voxel Y coords, origin at world Y=0):
	//   0 .. kStoneTop-1  → Stone
	//   kStoneTop .. kDirtTop-1 → Dirt
	//   kDirtTop  → Grass top face, Dirt sides+bottom
	static constexpr int kTotalHeight = voxel::kChunkSize * kGroundChunks;
	static constexpr int kBaseHeight = 10;
	static constexpr int kHillAmpA = 5;
	static constexpr int kHillAmpB = 3;
	static constexpr int kDirtDepth = 4;

	// ── UV helpers ────────────────────────────────────────────────────────────
	//
	// Atlas layout is an evenly sliced N×N grid.
	// Change kAtlasTilesPerAxis to switch from 4x4 to 8x8, etc.
	// Row 0 (top): Grass-side(0,0) | Grass-top(1,0) | Dirt(2,0) | Stone(3,0)
	// (Exact UVs are tuned to match whatever atlas you place at kAtlasPath.)

	static constexpr int kAtlasTilesPerAxis = 4;
	static constexpr float kTile = 1.0f / static_cast<float>(kAtlasTilesPerAxis);

	static constexpr voxel::FaceUV UV(int col, int row)
	{
		return {
			.uvMin = {       col * kTile,       row * kTile },
              .uvMax = { (col + 1) * kTile, (row + 1) * kTile }
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
		        .position = { 0.0f, 26.0f, 32.0f },
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
		auto heightAt = [](int worldX, int worldZ)
		{
			const float waveA = std::sin(static_cast<float>(worldX) * 0.08f) * static_cast<float>(kHillAmpA);
			const float waveB = std::cos(static_cast<float>(worldZ) * 0.06f) * static_cast<float>(kHillAmpB);
			const int height = kBaseHeight + static_cast<int>(waveA + waveB);
			return std::clamp(height, 3, kTotalHeight - 2);
		};

		// Fill a simple heightfield with grass on top, dirt below, stone deeper down.
		for (int cx = -kWorldRadius; cx <= kWorldRadius; ++cx)
			for (int cz = -kWorldRadius; cz <= kWorldRadius; ++cz)
			{
				for (int localZ = 0; localZ < voxel::kChunkSize; ++localZ)
					for (int localX = 0; localX < voxel::kChunkSize; ++localX)
					{
						const int worldX = cx * voxel::kChunkSize + localX;
						const int worldZ = cz * voxel::kChunkSize + localZ;
						const int surfaceY = heightAt(worldX, worldZ);

						for (int worldY = 0; worldY <= surfaceY; ++worldY)
						{
							voxel::BlockId id;
							if (worldY == surfaceY)
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
