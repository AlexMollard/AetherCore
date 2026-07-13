#include "assets/AssetDatabase.hpp"

#include <array>
#include <string_view>

#include "mesh/PrimitiveMeshes.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		// The single canonical primitive kind-name <-> enum table (the mapping was
		// previously duplicated across serializer, script exports and inspector).
		struct PrimitiveKind
		{
			std::string_view name;
			PrimitiveMesh mesh;
			std::string_view display;
		};

		constexpr std::array<PrimitiveKind, 5> kPrimitiveKinds = {{
		        {"cube", PrimitiveMesh::Cube, "Cube"},
		        {"sphere", PrimitiveMesh::Sphere, "Sphere"},
		        {"plane", PrimitiveMesh::Plane, "Plane"},
		        {"quad", PrimitiveMesh::Quad, "Quad"},
		        {"triangle", PrimitiveMesh::Triangle, "Triangle"},
		}};

		const PrimitiveKind* FindPrimitiveKind(std::string_view name)
		{
			for (const PrimitiveKind& kind: kPrimitiveKinds)
			{
				if (kind.name == name)
				{
					return &kind;
				}
			}
			return nullptr;
		}
	} // namespace

	AssetDatabase::AssetDatabase(PrimitiveMeshes& primitives)
	      : m_primitives(primitives)
	{
	}

	void AssetDatabase::RegisterBuiltinPrimitives()
	{
		for (const PrimitiveKind& kind: kPrimitiveKinds)
		{
			Register(MakePrimitiveMeshSource(std::string(kind.name)), std::string(kind.display));
		}
	}

	AssetId AssetDatabase::Register(const AssetSource& source, std::string displayName)
	{
		if (source.type == AssetType::Unknown || source.path.empty())
		{
			return AssetId{};
		}
		const AssetId id = ComputeAssetId(source);
		auto it = m_entries.find(id);
		if (it == m_entries.end())
		{
			Entry entry;
			entry.source = source;
			entry.displayName = displayName.empty() ? source.path : std::move(displayName);
			m_entries.emplace(id, std::move(entry));
			m_order.push_back(id);
		}
		else if (it->second.displayName.empty() && !displayName.empty())
		{
			it->second.displayName = std::move(displayName);
		}
		return id;
	}

	bool AssetDatabase::Contains(AssetId id) const
	{
		return m_entries.contains(id);
	}

	bool AssetDatabase::Describe(AssetId id, AssetSource& out) const
	{
		const auto it = m_entries.find(id);
		if (it == m_entries.end())
		{
			return false;
		}
		out = it->second.source;
		return true;
	}

	AssetType AssetDatabase::TypeOf(AssetId id) const
	{
		const auto it = m_entries.find(id);
		return it == m_entries.end() ? AssetType::Unknown : it->second.source.type;
	}

	std::string AssetDatabase::DisplayName(AssetId id) const
	{
		const auto it = m_entries.find(id);
		return it == m_entries.end() ? std::string{} : it->second.displayName;
	}

	std::uint32_t AssetDatabase::Generation(AssetId id) const
	{
		const auto it = m_entries.find(id);
		return it == m_entries.end() ? 0u : it->second.generation;
	}

	void AssetDatabase::Touch(AssetId id)
	{
		if (const auto it = m_entries.find(id); it != m_entries.end())
		{
			++it->second.generation;
		}
	}

	std::size_t AssetDatabase::TouchByPath(const std::string& path)
	{
		std::size_t touched = 0;
		for (auto& [id, entry]: m_entries)
		{
			if (entry.source.path == path)
			{
				++entry.generation;
				++touched;
			}
		}
		return touched;
	}

	void AssetDatabase::ResolveWorldMeshes(World& world)
	{
		for (auto&& [entity, mc]: world.GetRegistry().view<MeshComponent>().each())
		{
			// Back-fill the id from the stable MeshSource identity the first time.
			if (!mc.asset.IsValid())
			{
				const auto* src = world.GetRegistry().try_get<MeshSourceComponent>(entity);
				if (src == nullptr)
				{
					continue; // no stable identity to reference (e.g. a raw spawned mesh)
				}
				mc.asset = ComputeAssetId(src->kind == MeshSourceComponent::Kind::Primitive ? MakePrimitiveMeshSource(src->path) : MakeModelMeshSource(src->path, static_cast<int>(src->primitiveIndex)));
			}

			const std::uint32_t gen = Generation(mc.asset);
			if (gen == 0 || gen == mc.resolvedGeneration)
			{
				continue; // unknown asset, or already up to date
			}
			if (const Mesh* resolved = ResolveMesh(mc.asset))
			{
				mc.mesh = resolved;
				mc.resolvedGeneration = gen;
			}
		}
	}

	const Mesh* AssetDatabase::ResolveMesh(AssetId id) const
	{
		const auto it = m_entries.find(id);
		if (it == m_entries.end() || it->second.source.type != AssetType::Mesh)
		{
			return nullptr;
		}
		const AssetSource& source = it->second.source;
		if (source.builtin)
		{
			if (const PrimitiveKind* kind = FindPrimitiveKind(source.path))
			{
				return &m_primitives.Get(kind->mesh);
			}
			return nullptr;
		}
		// glTF model primitive: needs the app-layer model cache via the resolver.
		if (m_modelMeshResolver)
		{
			return m_modelMeshResolver(source.path, source.subIndex);
		}
		return nullptr;
	}

	void AssetDatabase::ForEach(AssetType type, const std::function<void(AssetId, const Entry&)>& fn) const
	{
		for (const AssetId id: m_order)
		{
			const auto it = m_entries.find(id);
			if (it == m_entries.end())
			{
				continue;
			}
			if (type == AssetType::Unknown || it->second.source.type == type)
			{
				fn(id, it->second);
			}
		}
	}
} // namespace aether
