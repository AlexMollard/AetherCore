#include "mesh/GrassMeshData.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include "assets/GltfAsset.hpp"
#include "io/FileSystem.hpp"

namespace aether::MeshGen
{
	namespace
	{
		constexpr std::size_t kMaxCandidates = 262144;
		constexpr std::size_t kMaxChunks = 65536;

		bool Finite(glm::vec3 value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		std::uint32_t Mix(std::uint32_t value)
		{
			value ^= value >> 16;
			value *= 0x7feb352du;
			value ^= value >> 15;
			value *= 0x846ca68bu;
			return value ^ (value >> 16);
		}

		float Random(std::uint32_t& state)
		{
			state = Mix(state + 0x9e3779b9u);
			return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
		}

		struct Triangle
		{
			glm::vec3 a;
			glm::vec3 b;
			glm::vec3 c;
			glm::vec3 ca, cb, cc;
			glm::vec3 normal;
			float inverseArea;
		};

		float Cross(glm::vec2 a, glm::vec2 b)
		{
			return a.x * b.y - a.y * b.x;
		}

		class SurfaceGrid
		{
		public:
			Expected<void> Build(GrassSurface surface, glm::vec2 regionMin, glm::vec2 regionMax)
			{
				const std::size_t count = surface.indices.empty() ? surface.vertices.size() : surface.indices.size();
				if (count % 3 != 0)
				{
					AE_UNEXPECTED(AetherError::Asset("Grass source must contain complete triangles"));
				}
				if (count == 0)
				{
					return {};
				}
				glm::vec2 sourceMin{std::numeric_limits<float>::max()};
				glm::vec2 sourceMax{-std::numeric_limits<float>::max()};
				for (const auto& vertex: surface.vertices)
				{
					if (!Finite(vertex.position))
					{
						AE_UNEXPECTED(AetherError::Asset("Grass source has an invalid vertex"));
					}
					const glm::vec2 point{vertex.position.x, vertex.position.z};
					sourceMin = glm::min(sourceMin, point);
					sourceMax = glm::max(sourceMax, point);
				}
				const glm::vec2 bounds = glm::max(glm::min(sourceMax, regionMax) - glm::max(sourceMin, regionMin), glm::vec2(0));
				const double sourceArea = static_cast<double>(bounds.x) * bounds.y;
				m_cell_size = static_cast<float>(std::max(1.0, std::sqrt(sourceArea * 2.0 / (count / 3))));
				for (std::size_t i = 0; i < count; i += 3)
				{
					std::array<glm::vec3, 3> points;
					std::array<glm::vec3, 3> colors;
					for (std::size_t j = 0; j < 3; ++j)
					{
						const auto index = surface.indices.empty() ? i + j : surface.indices[i + j];
						if (index >= surface.vertices.size() || !Finite(surface.vertices[index].position))
						{
							AE_UNEXPECTED(AetherError::Asset("Grass source has an invalid vertex or index"));
						}
						points[j] = surface.vertices[index].position;
						colors[j] = surface.vertices[index].color;
						if (!Finite(colors[j]))
						{
							AE_UNEXPECTED(AetherError::Asset("Grass source has an invalid vertex color"));
						}
					}
					const glm::vec2 a{points[0].x, points[0].z};
					const glm::vec2 b{points[1].x, points[1].z};
					const glm::vec2 c{points[2].x, points[2].z};
					const float area = Cross(b - a, c - a);
					if (!std::isfinite(area))
					{
						AE_UNEXPECTED(AetherError::Asset("Grass source triangle exceeds finite sampling range"));
					}
					if (std::abs(area) < 1e-8f)
					{
						continue;
					}
					const glm::vec2 lo = glm::min(a, glm::min(b, c));
					const glm::vec2 hi = glm::max(a, glm::max(b, c));
					if (hi.x < regionMin.x || hi.y < regionMin.y || lo.x > regionMax.x || lo.y > regionMax.y)
					{
						continue;
					}
					const glm::ivec2 first = Cell(glm::max(lo, regionMin));
					const glm::ivec2 last = Cell(glm::min(hi, regionMax));
					const bool oversized = static_cast<double>(last.x - static_cast<double>(first.x) + 1) * (last.y - static_cast<double>(first.y) + 1) > kMaxCandidates;
					const auto triangleIndex = static_cast<std::uint32_t>(m_triangles.size());
					auto normal = glm::normalize(glm::cross(points[1] - points[0], points[2] - points[0]));
					if (normal.y < 0.0f)
					{
						normal = -normal;
					}
					m_triangles.push_back({points[0], points[1], points[2], colors[0], colors[1], colors[2], normal, 1.0f / area});
					if (oversized)
					{
						m_large_triangles.push_back(triangleIndex);
						continue;
					}
					for (int z = first.y; z <= last.y; ++z)
					{
						for (int x = first.x; x <= last.x; ++x)
						{
							m_bins[BinKey(x, z)].push_back(triangleIndex);
						}
					}
				}
				return {};
			}

			struct Sample { float height; glm::vec3 color; glm::vec3 normal; };
			std::optional<Sample> Height(glm::vec2 position) const
			{
				const glm::ivec2 cell = Cell(position);
				std::optional<Sample> height;
				const auto bin = m_bins.find(BinKey(cell.x, cell.y));
				const auto sample = [&](std::uint32_t index)
				{
					const auto& t = m_triangles[index];
					const glm::vec2 a{t.a.x, t.a.z};
					const glm::vec2 b{t.b.x, t.b.z};
					const glm::vec2 c{t.c.x, t.c.z};
					const float u = Cross(position - a, c - a) * t.inverseArea;
					const float v = Cross(b - a, position - a) * t.inverseArea;
					if (u >= -1e-6f && v >= -1e-6f && u + v <= 1.000001f)
					{
						const float y = t.a.y + u * (t.b.y - t.a.y) + v * (t.c.y - t.a.y);
						if (!height || y > height->height)
						{
							height = Sample{y, t.ca + u * (t.cb - t.ca) + v * (t.cc - t.ca), t.normal};
						}
					}
				};
				if (bin != m_bins.end())
				{
					for (const auto index: bin->second)
					{
						sample(index);
					}
				}
				for (const auto index: m_large_triangles)
				{
					sample(index);
				}
				return height;
			}

		private:
			static std::uint64_t BinKey(int x, int z)
			{
				return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
			}
			glm::ivec2 Cell(glm::vec2 point) const
			{
				const glm::vec2 local = glm::floor(point / m_cell_size);
				return {static_cast<int>(local.x), static_cast<int>(local.y)};
			}

			float m_cell_size = 1.0f;
			std::vector<Triangle> m_triangles;
			std::vector<std::uint32_t> m_large_triangles;
			std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> m_bins;
		};

		Expected<MeshData> LoadSurface(const std::string& path)
		{
			if (path.empty())
			{
				return MeshData{};
			}
			AE_TRY(asset, assets::GltfAsset::LoadFromVfsPath(path));
			MeshData result;
			for (const auto& primitive: asset->primitives)
			{
				glm::mat4 transform{1.0f};
				if (!asset->nodes.empty())
				{
					std::int64_t index = primitive.nodeIndex;
					std::size_t depth = 0;
					while (index >= 0)
					{
						if (static_cast<std::size_t>(index) >= asset->nodes.size() || ++depth > asset->nodes.size())
						{
							AE_UNEXPECTED(AetherError::Asset("Grass source has an invalid node hierarchy"));
						}
						const auto& node = asset->nodes[static_cast<std::size_t>(index)];
						const auto local = node.hasMatrix ? node.matrix : glm::translate(glm::mat4{1.0f}, node.translation) * glm::mat4_cast(node.rotation) * glm::scale(glm::mat4{1.0f}, node.scale);
						transform = local * transform;
						index = node.parentIndex;
					}
				}
				if (asset->primitives.size() == 1)
				{
					auto& only = asset->primitives.front();
					result.vertices = std::move(only.vertices);
					result.indices = std::move(only.indices);
					if (result.vertices.empty())
					{
						AE_UNEXPECTED(AetherError::Asset("Grass source contains no mesh geometry"));
					}
					for (auto& vertex: result.vertices)
					{
						vertex.position = glm::vec3(transform * glm::vec4(vertex.position, 1.0f));
					}
					return result;
				}
				const auto base = static_cast<std::uint32_t>(result.vertices.size());
				for (const auto& vertex: primitive.vertices)
				{
					auto& output = result.vertices.emplace_back(vertex);
					output.position = glm::vec3(transform * glm::vec4(vertex.position, 1.0f));
				}
				if (primitive.indices.empty())
				{
					for (std::uint32_t i = 0; i < primitive.vertices.size(); ++i)
					{
						result.indices.push_back(base + i);
					}
				}
				else
				{
					for (const auto index: primitive.indices)
					{
						if (index >= primitive.vertices.size())
						{
							AE_UNEXPECTED(AetherError::Asset("Grass source has an invalid primitive index"));
						}
						result.indices.push_back(base + index);
					}
				}
			}
			if (result.vertices.empty())
			{
				AE_UNEXPECTED(AetherError::Asset("Grass source contains no mesh geometry"));
			}
			return result;
		}
	}

	struct GrassField::Impl
	{
		GrassComponent settings;
		SurfaceGrid ground, excluded;
		bool hasSurface = false;
	};

	GrassField::GrassField() = default;
	GrassField::~GrassField() = default;
	GrassField::GrassField(GrassField&&) noexcept = default;
	GrassField& GrassField::operator=(GrassField&&) noexcept = default;

	Expected<GrassField> GrassField::Create(const GrassComponent& s, GrassSurface surface, GrassSurface exclusions)
	{
		const glm::vec2 extent = s.regionMax - s.regionMin;
		const double coordinateLimit = static_cast<double>(std::numeric_limits<std::int32_t>::max()) - 128.0;
		if (!Finite({s.regionMin, 0.0f}) || !Finite({s.regionMax, 0.0f}) || !Finite({extent, 0.0f}) ||
		    !Finite({s.heightRange, 0.0f}) || !Finite({s.lodDistances, 0.0f}) || !Finite(s.baseColor) || !Finite(s.tipColor) ||
		    !std::isfinite(s.spacing) || !std::isfinite(s.chunkSize) || !std::isfinite(s.bladeWidth) ||
		    !std::isfinite(s.drawDistance) || !std::isfinite(s.windStrength) ||
		    extent.x <= 0 || extent.y <= 0 || s.spacing < 0.1f || s.chunkSize < s.spacing ||
		    s.heightRange.x <= 0 || s.heightRange.y < s.heightRange.x || s.bladeWidth <= 0 ||
		    s.lodDistances.x <= 0 || s.lodDistances.y < s.lodDistances.x || s.drawDistance < s.lodDistances.y || s.windStrength < 0 ||
		    std::max({std::abs(static_cast<double>(s.regionMin.x)), std::abs(static_cast<double>(s.regionMin.y)), std::abs(static_cast<double>(s.regionMax.x)), std::abs(static_cast<double>(s.regionMax.y))}) / s.spacing > coordinateLimit ||
		    std::pow(std::ceil(s.chunkSize / s.spacing) + 2.0, 2) > kMaxCandidates ||
		    std::pow(2.0 * std::ceil((s.drawDistance + 1.5) / s.chunkSize) + 3.0, 2) * 3.0 > kMaxChunks)
		{
			AE_UNEXPECTED(AetherError::Asset("Grass settings exceed finite bounds or per-chunk and camera-window limits"));
		}
		GrassField field;
		field.m_impl = std::make_unique<Impl>();
		field.m_impl->settings = s;
		field.m_impl->hasSurface = !surface.vertices.empty();
		AE_TRY_VOID(field.m_impl->ground.Build(surface, s.regionMin, s.regionMax));
		AE_TRY_VOID(field.m_impl->excluded.Build(exclusions, s.regionMin, s.regionMax));
		return field;
	}

	Expected<GrassField> GrassField::Load(const GrassComponent& settings)
	{
		AE_TRY(surface, LoadSurface(settings.surfaceMesh));
		AE_TRY(exclusions, LoadSurface(settings.exclusionMesh));
		return Create(settings, {surface->vertices, surface->indices}, {exclusions->vertices, exclusions->indices});
	}

	glm::vec2 GrassField::DistanceFade(std::uint32_t lod) const
	{
		if (!m_impl || lod > 2)
		{
			return {};
		}
		const auto& s = m_impl->settings;
		const float distance = lod == 0 ? s.lodDistances.x : lod == 1 ? s.lodDistances.y : s.drawDistance;
		return {distance * 0.75f, distance};
	}

	Expected<void> GrassField::SelectChunks(glm::vec2 camera, std::vector<GrassChunkKey>& selected, float localDistanceScale) const
	{
		selected.clear();
		if (!m_impl || !Finite({camera, 0.0f}) || !std::isfinite(localDistanceScale) || localDistanceScale <= 0)
		{
			AE_UNEXPECTED(AetherError::Asset("Grass selection requires a finite camera and positive finite distance scale"));
		}
		const auto& s = m_impl->settings;
		const glm::dvec2 center = glm::floor(glm::dvec2(camera));
		struct Window
		{
			std::int32_t x0, z0, x1, z1;
			double radius;
			bool active = false;
		};
		std::array<Window, 3> windows{};
		std::size_t candidates = 0;
		for (std::uint32_t lod = 0; lod < 3; ++lod)
		{
			const double radius = static_cast<double>(DistanceFade(lod).y) * localDistanceScale + std::max(1.5, 1.5 * localDistanceScale);
			const glm::dvec2 lo = glm::max(glm::dvec2(s.regionMin), center - radius);
			const glm::dvec2 hi = glm::min(glm::dvec2(s.regionMax), center + radius);
			if (lo.x >= hi.x || lo.y >= hi.y)
			{
				continue;
			}
			const auto x0 = static_cast<std::int32_t>(std::floor(lo.x / s.chunkSize));
			const auto z0 = static_cast<std::int32_t>(std::floor(lo.y / s.chunkSize));
			const auto x1 = static_cast<std::int32_t>(std::floor(hi.x / s.chunkSize));
			const auto z1 = static_cast<std::int32_t>(std::floor(hi.y / s.chunkSize));
			const double count = (static_cast<double>(x1) - x0 + 1) * (static_cast<double>(z1) - z0 + 1);
			if (count > kMaxChunks - candidates)
			{
				AE_UNEXPECTED(AetherError::Asset("Grass selection exceeds the camera-window candidate limit"));
			}
			candidates += static_cast<std::size_t>(count);
			windows[lod] = {x0, z0, x1, z1, radius, true};
		}
		for (std::uint32_t lod = 0; lod < 3; ++lod)
		{
			const auto& window = windows[lod];
			if (!window.active)
			{
				continue;
			}
			const auto x0 = window.x0, z0 = window.z0, x1 = window.x1, z1 = window.z1;
			const double radius = window.radius;
			for (auto z = z0; z <= z1; ++z)
			{
				for (auto x = x0; x <= x1; ++x)
				{
					const double dx = std::max({x * static_cast<double>(s.chunkSize) - center.x, 0.0, center.x - (x + 1.0) * s.chunkSize});
					const double dz = std::max({z * static_cast<double>(s.chunkSize) - center.y, 0.0, center.y - (z + 1.0) * s.chunkSize});
					if (dx * dx + dz * dz <= radius * radius)
					{
						selected.push_back({x, z, lod});
					}
				}
			}
		}
		std::sort(selected.begin(), selected.end(), [&](auto a, auto b) {
			const auto distance = [&](auto k) {
				const double x = (k.x + 0.5) * s.chunkSize - center.x, z = (k.z + 0.5) * s.chunkSize - center.y;
				return x * x + z * z;
			};
			const double da = distance(a), db = distance(b);
			return da == db ? a < b : da < db;
		});
		return {};
	}

	Expected<GrassChunk> GrassField::GenerateChunk(GrassChunkKey key) const
	{
		if (!m_impl || key.lod > 2)
		{
			AE_UNEXPECTED(AetherError::Asset("Invalid grass chunk"));
		}
		const auto& s = m_impl->settings;
		const double ox = static_cast<double>(key.x) * s.chunkSize, oz = static_cast<double>(key.z) * s.chunkSize;
		GrassChunk chunk;
		chunk.origin = {static_cast<float>(ox), 0.0f, static_cast<float>(oz)};
		const double x0 = std::max(ox, static_cast<double>(s.regionMin.x)), z0 = std::max(oz, static_cast<double>(s.regionMin.y));
		const double x1 = std::min(ox + s.chunkSize, static_cast<double>(s.regionMax.x)), z1 = std::min(oz + s.chunkSize, static_cast<double>(s.regionMax.y));
		if (x0 >= x1 || z0 >= z1)
		{
			return chunk;
		}
		const auto firstX = static_cast<std::int32_t>(std::floor(x0 / s.spacing)), firstZ = static_cast<std::int32_t>(std::floor(z0 / s.spacing));
		const auto lastX = static_cast<std::int32_t>(std::ceil(x1 / s.spacing)), lastZ = static_cast<std::int32_t>(std::ceil(z1 / s.spacing));
		const auto width = static_cast<std::size_t>(lastX - firstX);
		const auto depth = static_cast<std::size_t>(lastZ - firstZ);
		const auto multiples = [](std::int32_t first, std::int32_t last, int step) {
			const auto before = [step](std::int32_t n) {
				const auto quotient = n / step;
				return quotient - (n % step < 0 ? 1 : 0);
			};
			return static_cast<std::size_t>(before(last - 1) - before(first - 1));
		};
		const auto evenCells = multiples(firstX, lastX, 2) * multiples(firstZ, lastZ, 2);
		const auto fourthCells = multiples(firstX, lastX, 4) * multiples(firstZ, lastZ, 4);
		const auto capacity = key.lod == 0 ? width * depth - evenCells : key.lod == 1 ? evenCells - fourthCells : fourthCells;
		chunk.mesh.vertices.reserve(capacity * 5);
		chunk.mesh.indices.reserve(capacity * 9);
		for (auto z = firstZ; z < lastZ; ++z)
		{
			for (auto x = firstX; x < lastX; ++x)
			{
				const bool even = x % 2 == 0 && z % 2 == 0, fourth = x % 4 == 0 && z % 4 == 0;
				if ((key.lod == 0 && even) || (key.lod == 1 && (!even || fourth)) || (key.lod == 2 && !fourth))
				{
					continue;
				}
				std::uint32_t random = Mix(static_cast<std::uint32_t>(x) ^ Mix(static_cast<std::uint32_t>(z)) ^ Mix(s.seed));
				const float jx = 0.1f + 0.8f * Random(random), jz = 0.1f + 0.8f * Random(random);
				const glm::vec2 pos{static_cast<float>((x + static_cast<double>(jx)) * s.spacing), static_cast<float>((z + static_cast<double>(jz)) * s.spacing)};
				if (pos.x < x0 || pos.x >= x1 || pos.y < z0 || pos.y >= z1 || m_impl->excluded.Height(pos))
				{
					continue;
				}
				auto ground = m_impl->hasSurface ? m_impl->ground.Height(pos) : std::optional<SurfaceGrid::Sample>{SurfaceGrid::Sample{0, glm::vec3(1), glm::vec3(0, 1, 0)}};
				if (!ground || (s.surfaceMask && Random(random) >= glm::clamp(ground->color.r, 0.0f, 1.0f)))
				{
					continue;
				}
				const float height = glm::mix(s.heightRange.x, s.heightRange.y, Random(random)) * (s.surfaceMask ? glm::clamp(ground->color.g, 0.0f, 1.0f) : 1.0f);
				if (height <= 0)
				{
					continue;
				}
				const float yaw = Random(random) * 6.28318530718f;
				const glm::vec3 side{std::cos(yaw), 0, std::sin(yaw)}, lean{-side.z, 0, side.x};
				const float bend = height * (0.12f + 0.3f * Random(random));
				const glm::vec3 normal = glm::normalize(ground->normal * 0.35f + glm::vec3(0, 0.65f, 0));
				const glm::vec3 root{pos.x - chunk.origin.x, ground->height, pos.y - chunk.origin.z};
				const float patch = 0.5f + 0.25f * std::sin(pos.x * 0.21f + std::sin(pos.y * 0.13f)) + 0.25f * std::sin(pos.y * 0.17f - pos.x * 0.08f);
				const float dry = s.surfaceMask ? glm::clamp(ground->color.b, 0.0f, 1.0f) : patch;
				const glm::vec3 gold{0.75f, 0.63f, 0.25f};
				const glm::vec3 baseColor = glm::mix(s.baseColor, gold * 0.42f, dry * 0.55f), tipColor = glm::mix(s.tipColor, gold, dry * 0.55f);
				const auto base = static_cast<std::uint32_t>(chunk.mesh.vertices.size());
				const auto vertex = [&](glm::vec3 p, glm::vec2 uv, float anchor, glm::vec3 color) {
					chunk.mesh.vertices.push_back({.position = p, .normal = normal, .tangent = glm::vec4(side, 1), .uv = uv, .uv2 = {anchor, height}, .color = color});
				};
				vertex(root - side * s.bladeWidth * 0.5f, {0, 0}, 0, baseColor);
				vertex(root + side * s.bladeWidth * 0.5f, {1, 0}, 0, baseColor);
				const glm::vec3 middle = root + glm::vec3(0, height * 0.55f, 0) + lean * bend * 0.25f;
				vertex(middle - side * s.bladeWidth * 0.3f, {0, 0.55f}, 0.55f, glm::mix(baseColor, tipColor, 0.55f));
				vertex(middle + side * s.bladeWidth * 0.3f, {1, 0.55f}, 0.55f, glm::mix(baseColor, tipColor, 0.55f));
				vertex(root + glm::vec3(0, height, 0) + lean * bend, {0.5f, 1}, 1, tipColor);
				chunk.mesh.indices.insert(chunk.mesh.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2, base + 2, base + 3, base + 4});
			}
		}
		return chunk;
	}

	std::uint64_t GrassMeshKey(const GrassComponent& settings)
	{
		std::uint64_t hash = 14695981039346656037ull;
		const auto add = [&hash](std::uint64_t value)
		{
			for (int i = 0; i < 8; ++i)
			{
				hash = (hash ^ (value & 0xffu)) * 1099511628211ull;
				value >>= 8;
			}
		};
		add(2); // Geometry format version.
		for (const auto* path: {&settings.surfaceMesh, &settings.exclusionMesh})
		{
			add(path->size());
			for (const unsigned char byte: *path)
			{
				hash = (hash ^ byte) * 1099511628211ull;
			}
			if (!path->empty())
			{
				add(io::FileSystem::ContentStamp(*path));
				add(io::FileSystem::ContentStamp(assets::GltfAsset::ResolveMeshPath(*path)));
			}
		}
		add(settings.seed);
		add(settings.surfaceMask);
		for (const float value: {settings.regionMin.x, settings.regionMin.y, settings.regionMax.x, settings.regionMax.y, settings.spacing, settings.chunkSize, settings.heightRange.x, settings.heightRange.y,
		        settings.bladeWidth, settings.lodDistances.x, settings.lodDistances.y, settings.drawDistance, settings.baseColor.x, settings.baseColor.y, settings.baseColor.z, settings.tipColor.x, settings.tipColor.y, settings.tipColor.z})
		{
			add(std::bit_cast<std::uint32_t>(value));
		}
		return hash;
	}
}
