#pragma once

#include <compare>
#include <memory>
#include <span>
#include <vector>

#include "mesh/MeshGen.hpp"
#include "scene/GrassComponent.hpp"
#include "utils/Expected.hpp"

namespace aether::MeshGen
{
	struct GrassSurface
	{
		std::span<const MeshVertex> vertices;
		std::span<const std::uint32_t> indices;
	};

	struct GrassChunkKey
	{
		std::int32_t x = 0;
		std::int32_t z = 0;
		std::uint32_t lod = 0;
		auto operator<=>(const GrassChunkKey&) const = default;
	};

	struct GrassChunk
	{
		glm::vec3 origin{0.0f};
		MeshData mesh;
	};

	// Immutable prepared terrain and exclusions are shared by all streamed chunks of a field.
	class GrassField
	{
	public:
		GrassField();
		~GrassField();
		GrassField(GrassField&&) noexcept;
		GrassField& operator=(GrassField&&) noexcept;
		GrassField(const GrassField&) = delete;
		GrassField& operator=(const GrassField&) = delete;

		[[nodiscard]] static Expected<GrassField> Create(const GrassComponent& settings, GrassSurface surface = {}, GrassSurface exclusions = {});
		[[nodiscard]] static Expected<GrassField> Load(const GrassComponent& settings);
		[[nodiscard]] Expected<void> SelectChunks(glm::vec2 cameraLocal, std::vector<GrassChunkKey>& selected, float localDistanceScale = 1.0f) const;
		[[nodiscard]] Expected<GrassChunk> GenerateChunk(GrassChunkKey key) const;
		[[nodiscard]] glm::vec2 DistanceFade(std::uint32_t lod) const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

	// Includes geometry settings and source content stamps; suitable for resident mesh caches.
	[[nodiscard]] std::uint64_t GrassMeshKey(const GrassComponent& settings);
}
