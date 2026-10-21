#include <doctest/doctest.h>
#include <cstddef>
#include "gpu/SpritePass.hpp"
#include "gpu/AsyncRenderLifecycle.hpp"
#include "gpu/MeshPass.hpp"
#include <limits>
#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("WebGPU sprite bounds ordering and asynchronous retirement")
{
	using namespace aether;
	static_assert(sizeof(gpu::SpriteGpuInstance) == 128);
	static_assert(offsetof(gpu::SpriteGpuInstance, color) == 80);
	Render2DFrameData frame;
	for (unsigned i = 0; i < 7; ++i)
	{
		SpriteRenderInstance sprite;
		sprite.entityId = i;
		sprite.textureIndex = i < 4 ? 0 : 1;
		sprite.sortKey = 6 - i;
		frame.sprites.push_back(sprite);
	}
	auto plan = gpu::PlanSpritePass(frame, 2, 16);
	REQUIRE(plan.has_value());
	CHECK(plan->instances.size() == 7);
	CHECK(plan->batches.size() == 4);
	CHECK(plan->instances.front().metadata.y == 6);
	CHECK(plan->batches[0].texture == 1);
	CHECK(plan->batches[0].count == 2);
	CHECK(!gpu::PlanSpritePass(frame, 2, 6));
	CHECK(!gpu::PlanSpritePass(frame, 0, 16));
	frame.sprites[0].blendMode = 99;
	CHECK(!gpu::PlanSpritePass(frame, 2, 16));
	gpu::AsyncRenderLifecycle life;
	CHECK(life.State() == gpu::RenderState::Initializing);
	life.Stop();
	CHECK(!life.Ready());
	CHECK(life.State() == gpu::RenderState::Stopped);
	gpu::AsyncRenderLifecycle live;
	CHECK(live.Ready());
	CHECK(live.Submit());
	CHECK(live.Submit());
	live.Stop();
	CHECK(!live.Submit());
	CHECK(live.State() == gpu::RenderState::Stopping);
	live.Complete();
	CHECK(live.State() == gpu::RenderState::Stopping);
	live.Complete();
	CHECK(live.State() == gpu::RenderState::Stopped);
	gpu::AsyncRenderLifecycle lost;
	CHECK(lost.Ready());
	CHECK(lost.Submit());
	lost.Fail(true);
	CHECK(!lost.Submit());
	lost.Fail();
	lost.Complete();
	CHECK(lost.State() == gpu::RenderState::DeviceLost);
}

TEST_CASE("Portable sprite batches isolate display space UI from HDR scene draws")
{
	using namespace aether;
	Render2DFrameData frame;
	frame.sprites.push_back({.sortKey = 0});
	frame.sprites.push_back({.sortKey = 1, .flags = SpriteInstanceFlags::DisplaySpace});
	frame.sprites.push_back({.sortKey = 2, .flags = SpriteInstanceFlags::DisplaySpace});
	auto plan = gpu::PlanSpritePass(frame, 1024, 65536);
	REQUIRE(plan);
	REQUIRE(plan->batches.size() == 2);
	CHECK_FALSE(plan->batches[0].displaySpace);
	CHECK(plan->batches[0].count == 1);
	CHECK(plan->batches[1].displaySpace);
	CHECK(plan->batches[1].count == 2);
	CHECK(plan->batches[1].first == 1);
}

TEST_CASE("Portable mesh frames validate transforms and preserve draw order")
{
	using namespace aether;
	Render3DFrameData frame;
	frame.meshes.push_back({.world = glm::scale(glm::mat4(1), glm::vec3(-2, 4, 0.5f)), .color = {1, 0, 0, 1}, .mesh = 7});
	frame.meshes.push_back({.color = {0, 0, 1, 1}, .mesh = 3});
	const auto planned = gpu::PlanMeshPass(frame);
	REQUIRE(planned);
	CHECK(planned->size() == 2);
	CHECK(planned->at(0).normal0.x == doctest::Approx(-0.5f));
	CHECK(planned->at(0).normal1.y == doctest::Approx(0.25f));
	CHECK(planned->at(0).normal2.z == doctest::Approx(2));
	CHECK((planned->at(0).metadata.w & gpu::kMeshMirrored) != 0);
	CHECK(planned->at(1).color.b == 1);
	CHECK_FALSE(gpu::PlanMeshPass(frame, 1));
	frame.meshes[0].world[0][0] = 0;
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0].world = glm::mat4(1);
	frame.meshes[0].world[3][3] = 0;
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0].world = glm::mat4(1);
	frame.meshes[0].world[2][1] = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0].world = glm::mat4(1);
	frame.meshes[0].color.g = std::numeric_limits<float>::infinity();
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	CHECK(gpu::PlanMeshPass(Render3DFrameData{}));
}

TEST_CASE("Portable mesh materials retain factors and owned skin palette bounds")
{
	using namespace aether;
	static_assert(sizeof(gpu::MeshGpuInstance) == 192);
	static_assert(offsetof(gpu::MeshGpuInstance, metadata) == 176);
	Render3DFrameData frame;
	MeshRenderInstance mesh;
	mesh.color = {0.5f, 1, 1, 0.8f};
	mesh.material.baseColorFactor = {0.4f, 0.2f, 1, 0.5f};
	mesh.material.emissiveFactor = {1, 2, 3};
	mesh.material.alphaMask = true;
	mesh.material.modulateVertexColor = true;
	mesh.material.uvScroll = {0.5f, -1};
	mesh.skinMatrices = {glm::mat4(1), glm::mat4(2)};
	mesh.skinMatrices[1][3][3] = 1;
	frame.meshes = {mesh, mesh};
	auto plan = gpu::PlanMeshPass(frame);
	REQUIRE(plan);
	CHECK(plan->at(0).color.r == doctest::Approx(0.2f));
	CHECK(plan->at(0).color.a == doctest::Approx(0.4f));
	CHECK(plan->at(0).emissiveRoughness.z == 3);
	CHECK(plan->at(0).metadata.w == (GpuMaterial::kAlphaMask | GpuMaterial::kModulateVertexColor));
	CHECK(plan->at(1).metadata.y == 2);
	CHECK(plan->at(1).metadata.z == 2);
	CHECK(plan->at(0).uvScroll.y == -1);
	CHECK(frame.meshes[0].textures[0] == UINT32_MAX);
	CHECK(gpu::ValidMeshSkinInfluences({0, 1, 0, 0}, {0.25f, 0.75f, 0, 0}, 2));
	CHECK(gpu::ValidMeshSkinInfluences({0, UINT32_MAX, 0, 0}, {1, 0, 0, 0}, 2));
	CHECK_FALSE(gpu::ValidMeshSkinInfluences({0, UINT32_MAX, 0, 0}, {0.5f, 0.5f, 0, 0}, 2));
	CHECK_FALSE(gpu::ValidMeshSkinInfluences({0, 0, 0, 0}, {1, 1, 0, 0}, 1));
	CHECK_FALSE(gpu::ValidMeshSkinInfluences({0, 0, 0, 0}, {-1, 2, 0, 0}, 1));
	// Cooked static geometry may carry no influences; a palette draw cannot use it.
	CHECK(gpu::ValidMeshSkinInfluences({0, 0, 0, 0}, {0, 0, 0, 0}, 1, false));
	CHECK_FALSE(gpu::ValidMeshSkinInfluences({0, 0, 0, 0}, {0, 0, 0, 0}, 1));
	CHECK_FALSE(gpu::ValidMeshSkinInfluences({0, 0, 0, 0}, {0.25f, 0, 0, 0}, 1, false));
	CHECK(gpu::ValidMeshSkinInfluences({UINT32_MAX, 1, 0, 0}, {0, 0, 0, 0}, 1, false));
	CHECK(gpu::ValidMeshSkinInfluences({UINT32_MAX, 1, 0, 0}, {1, 0, 0, 0}, 1, false));
	CHECK_FALSE(gpu::ValidMeshSkinInfluences({UINT32_MAX, 1, 0, 0}, {1, 0, 0, 0}, 1));
	CHECK_FALSE(gpu::ValidMeshSkinInfluences({UINT32_MAX, 1, 0, 0}, {0, 0, 0, 0}, 1));
	frame.meshes[0].skinMatrices.resize(257, glm::mat4(1));
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0] = mesh;
	frame.meshes[0].skinMatrices[0][0][0] = std::numeric_limits<float>::infinity();
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0] = mesh;
	frame.meshes[0].material.alphaCutoff = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0] = mesh;
	frame.meshes[0].material.additiveBlend = true;
	CHECK_FALSE(gpu::PlanMeshPass(frame));
	frame.meshes[0].material.alphaBlend = true;
	CHECK(gpu::PlanMeshPass(frame));
	frame.meshes.assign(257, MeshRenderInstance{});
	for (auto& instance: frame.meshes)
	{
		instance.skinMatrices.assign(256, glm::mat4(1));
	}
	CHECK_FALSE(gpu::PlanMeshPass(frame));
}

TEST_CASE("Portable transparent mesh sorting is stable and preserves authored opaque order")
{
	using namespace aether;
	Render3DFrameData frame;
	for (unsigned i = 0; i < 6; ++i)
	{
		MeshRenderInstance mesh;
		mesh.world = glm::translate(glm::mat4(1), glm::vec3(0, 0, i == 5 ? -2 : -float(i)));
		mesh.material.alphaBlend = i % 2 == 1;
		frame.meshes.push_back(mesh);
	}
	const auto order = gpu::MeshDrawOrder(frame, glm::mat4(1));
	CHECK(order == std::vector<std::uint32_t>{0, 2, 4, 3, 5, 1});
	frame.meshes[5].world = frame.meshes[3].world;
	CHECK(gpu::MeshDrawOrder(frame, glm::mat4(1)) == std::vector<std::uint32_t>{0, 2, 4, 3, 5, 1});
}
