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

	// Central catalog of project assets keyed by stable AssetId. It interns asset
	// sources, tracks metadata plus a per-asset generation (bumped on reload for
	// hot-reload signalling), enumerates assets for pickers, and resolves mesh
	// assets to live Mesh pointers.
	//
	// Layering: built-in primitive resolution is engine-native (via PrimitiveMeshes).
	// glTF model-primitive resolution is delegated to an app-injected resolver,
	// because the model cache lives in the app-layer SceneContext - the database
	// stays engine-side and free of app dependencies.
	class AssetDatabase
	{
	public:
		struct Entry
		{
			AssetSource source;
			std::string displayName;
			std::uint32_t generation = 1; // bumped by Touch() on reload
		};

		// Resolves a glTF model primitive (vfsPath, primitiveIndex) to a live mesh.
		// Returns nullptr when the model can't be loaded. Injected by the app layer.
		using ModelMeshResolver = std::function<const Mesh*(const std::string& vfsPath, int primitiveIndex)>;

		explicit AssetDatabase(PrimitiveMeshes& primitives);

		// Register every built-in primitive as a Mesh asset. Call once after
		// PrimitiveMeshes::Initialize so the picker always lists the primitives.
		void RegisterBuiltinPrimitives();

		// Intern a source -> stable id (idempotent). The display name is stored the
		// first time an id is seen; later calls keep the existing entry. Passing an
		// Unknown/empty source returns the invalid id.
		AssetId Register(const AssetSource& source, std::string displayName = {});

		[[nodiscard]] bool Contains(AssetId id) const;
		[[nodiscard]] bool Describe(AssetId id, AssetSource& out) const;
		[[nodiscard]] AssetType TypeOf(AssetId id) const;
		[[nodiscard]] std::string DisplayName(AssetId id) const;
		[[nodiscard]] std::uint32_t Generation(AssetId id) const;

		// Bump an asset's generation so consumers know to re-resolve (hot-reload).
		// No-op for an unknown id.
		void Touch(AssetId id);

		// Bump the generation of every asset whose source path matches (a whole
		// model file reload touches all its primitives). Returns how many entries
		// were touched.
		std::size_t TouchByPath(const std::string& path);

		// Keeps every entity's MeshComponent.mesh in sync with its asset id:
		// back-fills the id from MeshSourceComponent the first time, then re-points
		// mesh whenever the asset's generation has bumped since the last resolve.
		// Cheap steady state (a generation compare per mesh); called once per frame.
		void ResolveWorldMeshes(World& world);

		// Resolve a Mesh asset (built-in primitive or model primitive) to a live
		// pointer, or nullptr if unknown / unresolved / not a mesh.
		[[nodiscard]] const Mesh* ResolveMesh(AssetId id) const;

		void SetModelMeshResolver(ModelMeshResolver resolver)
		{
			m_modelMeshResolver = std::move(resolver);
		}

		// Enumerate registered assets of a type (AssetType::Unknown == all) in
		// registration order.
		void ForEach(AssetType type, const std::function<void(AssetId, const Entry&)>& fn) const;

		[[nodiscard]] std::size_t Size() const
		{
			return m_order.size();
		}

	private:
		PrimitiveMeshes& m_primitives;
		ModelMeshResolver m_modelMeshResolver;
		std::unordered_map<AssetId, Entry> m_entries;
		std::vector<AssetId> m_order; // stable enumeration order
	};
} // namespace aether
