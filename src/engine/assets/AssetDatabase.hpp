#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/AssetId.hpp"
#include "assets/AssetTypes.hpp"

namespace aether
{
	class Mesh;
	class PrimitiveMeshes;
	class World;

	class AssetDatabase
	{
	public:
		struct Entry
		{
			AssetSource source;
			std::string displayName;
			std::uint32_t generation = 1;
		};

		using ModelMeshResolver = std::function<const Mesh*(const std::string& vfsPath, int primitiveIndex)>;

		explicit AssetDatabase(PrimitiveMeshes& primitives);

		void RegisterBuiltinPrimitives();

		AssetId Register(const AssetSource& source, std::string displayName = {});

		[[nodiscard]] bool Contains(AssetId id) const;
		[[nodiscard]] bool Describe(AssetId id, AssetSource& out) const;
		[[nodiscard]] AssetType TypeOf(AssetId id) const;
		[[nodiscard]] std::string DisplayName(AssetId id) const;
		[[nodiscard]] std::uint32_t Generation(AssetId id) const;

		void Touch(AssetId id);

		std::size_t TouchByPath(const std::string& path);

		void ResolveWorldMeshes(World& world);

		[[nodiscard]] const Mesh* ResolveMesh(AssetId id) const;

		void SetModelMeshResolver(ModelMeshResolver resolver)
		{
			m_modelMeshResolver = std::move(resolver);
		}

		void ForEach(AssetType type, const std::function<void(AssetId, const Entry&)>& fn) const;

		[[nodiscard]] std::size_t Size() const
		{
			return m_order.size();
		}

	private:
		PrimitiveMeshes& m_primitives;
		ModelMeshResolver m_modelMeshResolver;
		std::unordered_map<AssetId, Entry> m_entries;
		std::vector<AssetId> m_order;
	};
} // namespace aether
