#include "mesh/MeshGen.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include "utils/Profiler.hpp"

namespace aether::MeshGen
{
	MeshData GeneratePlane(const PlaneDesc& desc)
	{
		AE_PROFILE_ZONE();
		const int nx = std::max(desc.segmentsX, 1);
		const int ny = std::max(desc.segmentsY, 1);

		MeshData data;
		data.vertices.reserve((static_cast<std::size_t>(nx) + 1u) * (static_cast<std::size_t>(ny) + 1u));

		for (int row = 0; row <= ny; ++row)
		{
			for (int col = 0; col <= nx; ++col)
			{
				const float x = static_cast<float>(col) / static_cast<float>(nx) - 0.5f;
				const float y = static_cast<float>(row) / static_cast<float>(ny) - 0.5f;
				const float u = static_cast<float>(col) * desc.uvScale;
				const float v = static_cast<float>(ny - row) * desc.uvScale;

				data.vertices.push_back({
				        .position = {x, y, 0.0f},
				        .normal = {0.0f, 0.0f, 1.0f},
				        .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
				        .uv = {u, v},
				        .color = {1.0f, 1.0f, 1.0f},
				});
			}
		}

		data.indices.reserve(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) * 6u);
		for (int row = 0; row < ny; ++row)
		{
			for (int col = 0; col < nx; ++col)
			{
				const auto bl = static_cast<std::uint32_t>(row * (nx + 1) + col);
				const auto br = bl + 1u;
				const auto tl = static_cast<std::uint32_t>((row + 1) * (nx + 1) + col);
				const auto tr = tl + 1u;
				data.indices.push_back(bl);
				data.indices.push_back(br);
				data.indices.push_back(tr);
				data.indices.push_back(bl);
				data.indices.push_back(tr);
				data.indices.push_back(tl);
			}
		}

		return data;
	}

	MeshData GenerateUVSphere(const UVSphereDesc& desc)
	{
		AE_PROFILE_ZONE();
		const int stacks = std::max(desc.stacks, 2);
		const int slices = std::max(desc.slices, 3);

		// the same position but different UVs - required for correct wrapping).
		MeshData data;
		data.vertices.reserve((static_cast<std::size_t>(stacks) + 1u) * (static_cast<std::size_t>(slices) + 1u));

		for (int s = 0; s <= stacks; ++s)
		{
			const float phi = glm::pi<float>() * static_cast<float>(s) / static_cast<float>(stacks);
			const float sinPhi = std::sin(phi);
			const float cosPhi = std::cos(phi);

			for (int t = 0; t <= slices; ++t)
			{
				const float theta = glm::two_pi<float>() * static_cast<float>(t) / static_cast<float>(slices);
				const float sinTheta = std::sin(theta);
				const float cosTheta = std::cos(theta);

				const float x = 0.5f * sinPhi * cosTheta;
				const float y = 0.5f * cosPhi;
				const float z = 0.5f * sinPhi * sinTheta;

				const glm::vec3 normal = (sinPhi > 1e-6f) ? glm::vec3(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta) : glm::vec3(0.0f, cosPhi, 0.0f);

				const glm::vec3 tangentXYZ = (sinPhi > 1e-6f) ? glm::vec3(-sinTheta, 0.0f, cosTheta) : glm::vec3(1.0f, 0.0f, 0.0f);

				const float u = static_cast<float>(t) / static_cast<float>(slices) * desc.uvScale;
				const float v = static_cast<float>(s) / static_cast<float>(stacks) * desc.uvScale;

				data.vertices.push_back({
				        .position = {x, y, z},
				        .normal = normal,
				        .tangent = {tangentXYZ.x, tangentXYZ.y, tangentXYZ.z, 1.0f},
				        .uv = {u, v},
				        .color = {1.0f, 1.0f, 1.0f},
				});
			}
		}

		data.indices.reserve(static_cast<std::size_t>(stacks) * static_cast<std::size_t>(slices) * 6u);
		for (int s = 0; s < stacks; ++s)
		{
			for (int t = 0; t < slices; ++t)
			{
				const auto tl = static_cast<std::uint32_t>(s * (slices + 1) + t);
				const auto tr = tl + 1u;
				const auto bl = static_cast<std::uint32_t>((s + 1) * (slices + 1) + t);
				const auto br = bl + 1u;
				if (s != 0)
				{
					data.indices.push_back(tl);
					data.indices.push_back(tr);
					data.indices.push_back(bl);
				}
				if (s != stacks - 1)
				{
					data.indices.push_back(tr);
					data.indices.push_back(br);
					data.indices.push_back(bl);
				}
			}
		}

		return data;
	}

	MeshData GenerateCylinder(const CylinderDesc& desc)
	{
		AE_PROFILE_ZONE();
		const int n = std::max(desc.segments, 3);

		MeshData data;

		const int ringSize = n + 1;
		data.vertices.reserve(static_cast<std::size_t>(ringSize) * 2u + (desc.caps ? (static_cast<std::size_t>(n) + 1u) * 2u : 0u));

		for (int ring = 0; ring < 2; ++ring)
		{
			const float y = (ring == 0) ? -0.5f : 0.5f;
			const float v = static_cast<float>(ring) * desc.uvScaleAxial;

			for (int i = 0; i <= n; ++i)
			{
				const float theta = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(n);
				const float cosTheta = std::cos(theta);
				const float sinTheta = std::sin(theta);

				const float x = 0.5f * cosTheta;
				const float z = 0.5f * sinTheta;

				const glm::vec3 normal = {cosTheta, 0.0f, sinTheta};
				const glm::vec3 tangent = {-sinTheta, 0.0f, cosTheta};
				const float u = static_cast<float>(i) / static_cast<float>(n) * desc.uvScaleRadial;

				data.vertices.push_back({
				        .position = {x, y, z},
				        .normal = normal,
				        .tangent = {tangent.x, tangent.y, tangent.z, 1.0f},
				        .uv = {u, v},
				        .color = {1.0f, 1.0f, 1.0f},
				});
			}
		}

		data.indices.reserve(static_cast<std::size_t>(n) * 6u + (desc.caps ? static_cast<std::size_t>(n) * 3u * 2u : 0u));

		for (int i = 0; i < n; ++i)
		{
			const auto bl = static_cast<std::uint32_t>(i);
			const auto br = bl + 1u;
			const auto tl = static_cast<std::uint32_t>(ringSize + i);
			const auto tr = tl + 1u;
			data.indices.push_back(bl);
			data.indices.push_back(tl);
			data.indices.push_back(tr);
			data.indices.push_back(bl);
			data.indices.push_back(tr);
			data.indices.push_back(br);
		}

		if (desc.caps)
		{
			for (int cap = 0; cap < 2; ++cap)
			{
				const float y = (cap == 0) ? -0.5f : 0.5f;
				const float normalY = (cap == 0) ? -1.0f : 1.0f;

				const auto centerIdx = static_cast<std::uint32_t>(data.vertices.size());
				data.vertices.push_back({
				        .position = {0.0f, y, 0.0f},
				        .normal = {0.0f, normalY, 0.0f},
				        .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
				        .uv = {0.5f, 0.5f},
				        .color = {1.0f, 1.0f, 1.0f},
				});

				const auto rimStart = static_cast<std::uint32_t>(data.vertices.size());
				for (int i = 0; i <= n; ++i)
				{
					const float theta = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(n);
					const float cosTheta = std::cos(theta);
					const float sinTheta = std::sin(theta);

					const float x = 0.5f * cosTheta;
					const float z = 0.5f * sinTheta;
					const float u = cosTheta * 0.5f + 0.5f;
					const float v = sinTheta * 0.5f + 0.5f;

					data.vertices.push_back({
					        .position = {x, y, z},
					        .normal = {0.0f, normalY, 0.0f},
					        .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
					        .uv = {u, v},
					        .color = {1.0f, 1.0f, 1.0f},
					});
				}

				for (int i = 0; i < n; ++i)
				{
					const auto a = rimStart + static_cast<std::uint32_t>(i);
					const auto b = rimStart + static_cast<std::uint32_t>(i + 1);
					if (cap == 1)
					{
						data.indices.push_back(centerIdx);
						data.indices.push_back(a);
						data.indices.push_back(b);
					}
					else
					{
						data.indices.push_back(centerIdx);
						data.indices.push_back(b);
						data.indices.push_back(a);
					}
				}
			}
		}

		return data;
	}

} // namespace aether::MeshGen
