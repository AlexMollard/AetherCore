#pragma once

#if AE_WEB_TEST_BRIDGE
#	include <algorithm>
#	include <array>
#	include <emscripten.h>
#	include <glm/gtc/matrix_transform.hpp>
#	include "mesh/PrimitiveMeshData.hpp"
#	include "assets/GltfAsset.hpp"
#	include "rendering/RenderFramePacket.hpp"
#	include "scene/World.hpp"
#	include "scene/CameraComponents.hpp"
#	include "scene/Hierarchy.hpp"
#	include "webgpu/WebGpuDevice.hpp"

namespace aether::app::web
{
	inline bool IsMeshFixture()
	{
		return EM_ASM_INT({ return new URLSearchParams(location.search).get('scene') == 'mesh3d'; });
	}

	inline bool CreateMeshFixture(World& world, webgpu::WebGpuDevice& graphics)
	{
		const auto quad = GeneratePrimitiveMesh(PrimitiveMesh::Quad);
		const std::array<std::uint32_t, 3> badIndices{0, 1, 999};
		if (!quad || graphics.UploadMesh(1024, quad->vertices, quad->indices) || graphics.UploadMesh(0, quad->vertices, badIndices))
		{
			return false;
		}
		for (const auto primitive: {PrimitiveMesh::Quad, PrimitiveMesh::Cube})
		{
			const auto data = GeneratePrimitiveMesh(primitive);
			if (!data || !graphics.UploadMesh(static_cast<unsigned>(primitive), data->vertices, data->indices))
			{
				return false;
			}
		}
		if (graphics.UploadMesh(static_cast<unsigned>(PrimitiveMesh::Quad), quad->vertices, quad->indices))
		{
			return false;
		}
		const auto cooked = assets::GltfAsset::LoadFromVfsPath("project://assets/models/parity.gltf");
		if (!cooked || cooked->primitives.size() != 1 || !graphics.UploadMesh(7, cooked->primitives[0].vertices, cooked->primitives[0].indices))
		{
			return false;
		}
		const auto add = [&](std::string name, PrimitiveMesh mesh, glm::vec3 position, glm::vec3 scale, glm::vec4 color)
		{
			const auto entity = world.Create();
			world.Emplace<NameComponent>(entity, NameComponent{std::move(name)});
			world.Emplace<TransformComponent>(entity).localToWorld = glm::scale(glm::translate(glm::mat4(1), position), scale);
			world.Emplace<MeshSourceComponent>(entity, MeshSourceComponent{.primitiveIndex = static_cast<unsigned>(mesh)});
			world.Emplace<MeshRendererComponent>(entity);
			world.Emplace<MaterialInstanceComponent>(entity).asset.baseColorFactor = color;
		};
		add("A_Near", PrimitiveMesh::Quad, {0, 0, -2}, {1, 1, 1}, {1, 0, 0, 1});
		// Slot 7 is produced by the native glTF cooker and decoded by the same CPU reader.
		for (const auto raw: world.View<NameComponent, MeshSourceComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (world.Get<NameComponent>(entity).name == "A_Near")
			{
				world.Get<MeshSourceComponent>(entity).primitiveIndex = 7;
			}
		}
		add("B_Far", PrimitiveMesh::Quad, {0, 0, -4}, {4, 4, 1}, {0, 0, 1, 1});
		add("C_Cube", PrimitiveMesh::Cube, {3, 0, -4}, {-1, 2, 1}, {1, 1, 1, 1});
		const auto overlay = world.Create();
		world.Emplace<TransformComponent>(overlay).localToWorld = glm::translate(glm::mat4(1), glm::vec3(0, 3, -8));
		auto& sprite = world.Emplace<SpriteRendererComponent>(overlay);
		sprite.pixelSize = {1, 1};
		sprite.pixelsPerUnit = 1;
		sprite.tint = {0, 1, 0, 1};
		sprite.texturePath = "white";
		return true;
	}

	inline void ExtractMeshFixture(World& world, RenderFramePacket& packet, unsigned width, unsigned height)
	{
		std::erase_if(packet.render2D.sprites, [](const auto& sprite) { return sprite.textureIndex != 1; });
		packet.proj = glm::perspectiveRH_ZO(glm::radians(90.0f), float(width) / std::max(height, 1u), 0.1f, 100.0f);
		packet.proj[1][1] *= -1;
		std::vector<std::pair<std::string, MeshRenderInstance>> draws;
		for (const auto raw: world.View<TransformComponent, MeshSourceComponent, MeshRendererComponent, MaterialInstanceComponent, NameComponent>())
		{
			const auto entity = World::FromEntt(raw);
			if (!world.Get<MeshRendererComponent>(entity).visible || world.Has<DisabledComponent>(entity) || ecs::HasDisabledAncestor(world, entity))
			{
				continue;
			}
			draws.emplace_back(world.Get<NameComponent>(entity).name,
			        MeshRenderInstance{.world = world.Get<TransformComponent>(entity).localToWorld, .color = world.Get<MaterialInstanceComponent>(entity).asset.baseColorFactor, .mesh = world.Get<MeshSourceComponent>(entity).primitiveIndex});
		}
		std::sort(draws.begin(), draws.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
		for (const auto& [name, draw]: draws)
		{
			packet.render3D.meshes.push_back(draw);
		}
		packet.hasSceneDraws = !packet.render3D.meshes.empty();
		if (EM_ASM_INT({ return new URLSearchParams(location.search).has('badMesh'); }) && !packet.render3D.meshes.empty())
		{
			packet.render3D.meshes.front().mesh = 999;
		}
	}
} // namespace aether::app::web
#endif
