#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "BlockDef.hpp"

namespace aether
{
	class AetherCore;
	class Texture;
} // namespace aether

namespace voxel
{
	// Block registry — holds the definition and atlas UV data for every block type.
	//
	// At startup, call:
	//   BlockRegistry reg;
	//   reg.Initialize(core, "assets://textures/blocks/atlas.png");
	//   reg.Register(BlockId::Dirt,  { .uvMin={0,0}, .uvMax={0.125f,0.125f} }, ...);
	//
	// The atlas texture is loaded once and registered with the engine bindless system.
	// The atlas bindless slot is then used as the materialIndex on every chunk DrawCommand.
	class BlockRegistry
	{
	public:
		static constexpr std::uint32_t kMaxBlocks = static_cast<std::uint32_t>(BlockId::Count_);

		// Initialize and load the atlas texture.
		// Returns the bindless slot of the atlas for use in DrawCommand.materialIndex.
		std::uint32_t Initialize(aether::AetherCore& core, std::string_view atlasPath);
		void Shutdown(aether::AetherCore& core);

		// Register a block type with per-face UV rects.
		void Register(BlockId id, const BlockDef& def);

		// Convenience: register a block where all 6 faces use the same UV rect.
		void Register(BlockId id, FaceUV uniformFace, bool opaque = true);

		// Convenience: register a block with separate top/side/bottom UV rects.
		void Register(BlockId id, FaceUV top, FaceUV side, FaceUV bottom, bool opaque = true);

		[[nodiscard]] const BlockDef& Get(BlockId id) const;
		[[nodiscard]] bool IsOpaque(BlockId id) const;

		// Bindless slot of the atlas texture (for DrawCommand.materialIndex).
		[[nodiscard]] std::uint32_t GetAtlasSlot() const
		{
			return m_atlasSlot;
		}

	private:
		std::array<BlockDef, kMaxBlocks> m_defs{};
		std::uint32_t m_atlasSlot = 0xFFFFFFFFu;
		aether::Texture* m_atlasTexture = nullptr; // heap-allocated to stay in place
	};
} // namespace voxel
