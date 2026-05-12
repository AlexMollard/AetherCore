#include "BlockRegistry.hpp"

#include <cassert>

#include "ServiceContainer.hpp"
#include "io/FileSystem.hpp"
#include "material/Material.hpp"
#include "material/Texture.hpp"
#include "utils/AssetManager.hpp"
#include "utils/Logger.hpp"

namespace voxel
{
	std::uint32_t BlockRegistry::Initialize(ServiceContainer& services, std::string_view atlasPath)
	{
		aether::AssetManager& assetManager = services.Get<aether::AssetManager>();

		aether::Material mat{};

		if (aether::io::FileSystem::Exists(atlasPath))
		{
			m_atlasTexture = new aether::Texture(assetManager.CreateTexture(atlasPath, aether::TextureFilter::Nearest));
			mat.albedoSlot = m_atlasTexture->GetBindlessSlot();
		}
		else
		{
			//WARN(LogCategory::App, "Block atlas not found: {} - using fallback colour.", atlasPath);
		}

		assetManager.RegisterMaterial(mat);
		m_atlasSlot = mat.materialSlot;
		return m_atlasSlot;
	}

	void BlockRegistry::Shutdown(ServiceContainer& services)
	{
		(void) services;
		if (m_atlasTexture)
		{
			delete m_atlasTexture;
			m_atlasTexture = nullptr;
		}
	}

	void BlockRegistry::Register(BlockId id, const BlockDef& def)
	{
		const auto idx = static_cast<std::uint32_t>(id);
		assert(idx < kMaxBlocks && "BlockId out of range");
		m_defs[idx] = def;
	}

	void BlockRegistry::Register(BlockId id, FaceUV uniformFace, bool opaque)
	{
		BlockDef def{};
		def.opaque = opaque;
		def.faces.fill(uniformFace);
		Register(id, def);
	}

	void BlockRegistry::Register(BlockId id, FaceUV top, FaceUV side, FaceUV bottom, bool opaque)
	{
		BlockDef def{};
		def.opaque = opaque;
		def.faces[static_cast<std::uint32_t>(Face::PosY)] = top;
		def.faces[static_cast<std::uint32_t>(Face::NegY)] = bottom;
		def.faces[static_cast<std::uint32_t>(Face::PosX)] = side;
		def.faces[static_cast<std::uint32_t>(Face::NegX)] = side;
		def.faces[static_cast<std::uint32_t>(Face::PosZ)] = side;
		def.faces[static_cast<std::uint32_t>(Face::NegZ)] = side;
		Register(id, def);
	}

	const BlockDef& BlockRegistry::Get(BlockId id) const
	{
		const auto idx = static_cast<std::uint32_t>(id);
		assert(idx < kMaxBlocks && "BlockId out of range");
		return m_defs[idx];
	}

	bool BlockRegistry::IsOpaque(BlockId id) const
	{
		return Get(id).opaque;
	}
} // namespace voxel
