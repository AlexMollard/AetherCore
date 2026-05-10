#include "VoxelWorldLayer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <glm/glm.hpp>

#include <imgui.h>

#include "AetherCore.hpp"
#include "Camera.hpp"
#include "CameraManager.hpp"
#include "Logger.hpp"
#include "RenderQueue.hpp"

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
		{
			context.cameras->Destroy(m_camera);
		}

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
		static constexpr ImVec4 kGood{ 0.40f, 0.72f, 0.46f, 1.f };
		static constexpr ImVec4 kWarn{ 0.86f, 0.71f, 0.30f, 1.f };

		ImGui::SetNextWindowPos(ImVec2(12.f, 12.f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("VOXEL WORLD", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse))
		{
			ImGui::End();
			return;
		}

		ImGui::SeparatorText("WORLDGEN");
		ImGui::Columns(2, "##wg", false);

		ImGui::Text("Radius (chunks)");
		ImGui::NextColumn();
		ImGui::Text("%d", kWorldRadius);
		ImGui::NextColumn();
		ImGui::Text("Target XY chunks");
		ImGui::NextColumn();
		ImGui::Text("%d", (kWorldRadius * 2 + 1) * (kWorldRadius * 2 + 1));
		ImGui::NextColumn();
		ImGui::Text("Chunk size");
		ImGui::NextColumn();
		ImGui::Text("%d", voxel::kChunkSize);
		ImGui::NextColumn();
		ImGui::Text("Max terrain Y");
		ImGui::NextColumn();
		ImGui::Text("%d", kTotalHeight);
		ImGui::NextColumn();

		const aether::Camera* cam = context.cameras ? context.cameras->TryGet(m_camera) : nullptr;
		if (cam)
		{
			const glm::vec3 p = cam->GetPosition();
			ImGui::Text("Camera pos");
			ImGui::NextColumn();
			ImGui::Text("%.1f, %.1f, %.1f", p.x, p.y, p.z);
			ImGui::NextColumn();
		}

		ImGui::Columns(1);
		ImGui::SeparatorText("CHUNK RUNTIME");
		ImGui::Columns(2, "##cr", false);

		const voxel::ChunkManager::DebugStats stats = m_chunkManager.GetDebugStats();

		ImGui::Text("Total chunks");
		ImGui::NextColumn();
		ImGui::Text("%zu", stats.totalChunks);
		ImGui::NextColumn();

		ImGui::Text("Ready chunks");
		ImGui::NextColumn();
		ImGui::TextColored(stats.readyChunks > 1 ? kGood : kWarn, "%zu", stats.readyChunks);
		ImGui::NextColumn();

		ImGui::Text("Dirty chunks");
		ImGui::NextColumn();
		ImGui::TextColored(stats.dirtyChunks == 0 ? kGood : kWarn, "%zu", stats.dirtyChunks);
		ImGui::NextColumn();

		ImGui::Text("Empty chunks");
		ImGui::NextColumn();
		ImGui::Text("%zu", stats.emptyChunks);
		ImGui::NextColumn();

		ImGui::Text("Submitted draws");
		ImGui::NextColumn();
		ImGui::TextColored(stats.submittedDrawsLastFrame > 1 ? kGood : kWarn, "%u", stats.submittedDrawsLastFrame);
		ImGui::NextColumn();

		const bool bypass = context.engine.GetRenderQueue().IsDebugBypassIndirect();
		ImGui::Text("Draw path");
		ImGui::NextColumn();
		ImGui::TextColored(bypass ? kWarn : kGood, bypass ? "Bypass indirect" : "Indirect");
		ImGui::NextColumn();

		ImGui::Text("Rebuild A/U/F");
		ImGui::NextColumn();
		ImGui::TextColored(stats.rebuildFailuresLastFrame == 0 ? kGood : kWarn, "%u / %u / %u", stats.rebuildAttemptsLastFrame, stats.rebuildUploadsLastFrame, stats.rebuildFailuresLastFrame);
		ImGui::NextColumn();

		ImGui::Columns(1);
		ImGui::End();
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
