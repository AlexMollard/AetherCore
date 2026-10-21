#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace aether
{
	struct MeshRenderInstance
	{
		glm::mat4 world{1.0f};
		glm::vec4 color{1.0f};
		std::uint32_t mesh = 0;
	};

	struct Render3DFrameData
	{
		std::vector<MeshRenderInstance> meshes;
	};
} // namespace aether

namespace aether::gpu
{
	struct MeshGpuInstance
	{
		glm::mat4 world{1.0f};
		glm::vec4 normal0{}, normal1{}, normal2{};
		glm::vec4 color{1.0f};
	};

	static_assert(sizeof(MeshGpuInstance) == 128);
	static_assert(offsetof(MeshGpuInstance, color) == 112);

	[[nodiscard]] inline std::expected<std::vector<MeshGpuInstance>, std::string> PlanMeshPass(const Render3DFrameData& frame, std::size_t limit = 4096)
	{
		if (frame.meshes.size() > limit)
		{
			return std::unexpected("Mesh instance frame budget exceeded");
		}
		std::vector<MeshGpuInstance> instances;
		instances.reserve(frame.meshes.size());
		for (const auto& mesh: frame.meshes)
		{
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(mesh.world[column][row]))
					{
						return std::unexpected("Nonfinite mesh transform");
					}
				}
				if (!std::isfinite(mesh.color[column]))
				{
					return std::unexpected("Nonfinite mesh color");
				}
			}
			if (mesh.world[0].w != 0 || mesh.world[1].w != 0 || mesh.world[2].w != 0 || mesh.world[3].w != 1)
			{
				return std::unexpected("Mesh world transform must be affine");
			}
			const auto determinant = glm::determinant(glm::mat3(mesh.world));
			if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f)
			{
				return std::unexpected("Singular mesh transform");
			}
			const auto normal = glm::inverseTranspose(glm::mat3(mesh.world));
			for (int column = 0; column < 3; ++column)
			{
				for (int row = 0; row < 3; ++row)
				{
					if (!std::isfinite(normal[column][row]))
					{
						return std::unexpected("Nonfinite mesh normal transform");
					}
				}
			}
			instances.push_back({mesh.world, glm::vec4(normal[0], 0), glm::vec4(normal[1], 0), glm::vec4(normal[2], 0), mesh.color});
		}
		return instances;
	}
} // namespace aether::gpu
