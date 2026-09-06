// Ragdoll skin-drive math: the CPU-only half of driving a live skinned mesh's joints
// from a ragdoll's physics bodies (see src/engine/rendering/RagdollSkinDrive.hpp). No
// GPU/Vulkan context is needed to test this - node_flatten.slang only substitutes
// whatever BuildRagdollSkinOverrides already computed into globalTransforms verbatim,
// so pinning this function's output against a synthetic bind pose is exactly as
// strong a guarantee as reading the shader-written buffer back would be, without
// needing a live device.
#include <doctest/doctest.h>

#include <string>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics/PhysicsComponents.hpp"
#include "rendering/RagdollSkinDrive.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
	// A one-bone "ragdoll": a root entity naming a single bone via RagdollComponent,
	// and that bone's RagdollBoneComponent/TransformComponent set up exactly the way
	// RagdollBuilder.cpp computes them at spawn - skinNodeOffset derived from a KNOWN
	// bind pose (nodeBindWorld) and a KNOWN body bind pose, so every case below checks
	// against a value computed independently of BuildRagdollSkinOverrides itself.
	struct OneBoneRagdoll
	{
		World world;
		Entity root;
		Entity bone;
		glm::mat4 nodeBindWorld;

		OneBoneRagdoll(glm::vec3 nodeBindPos, glm::quat nodeBindRot, glm::vec3 bodyBindPos, glm::quat bodyBindRot)
		{
			nodeBindWorld = glm::translate(glm::mat4(1.0f), nodeBindPos) * glm::mat4_cast(nodeBindRot);
			const glm::mat4 bodyBindWorld = glm::translate(glm::mat4(1.0f), bodyBindPos) * glm::mat4_cast(bodyBindRot);
			const glm::mat4 skinNodeOffset = glm::inverse(bodyBindWorld) * nodeBindWorld;

			root = world.Create();
			bone = world.Create();
			world.Emplace<TransformComponent>(bone).localToWorld = bodyBindWorld;
			world.Emplace<RagdollBoneComponent>(bone,
			        RagdollBoneComponent{
			                .root = root,
			                .boneName = "TestBone",
			                .skinNodeName = "mixamorig:TestNode",
			                .skinNodeOffset = skinNodeOffset,
			        });
			world.Emplace<RagdollComponent>(root, RagdollComponent{.bones = {bone}});
		}

		void MoveBoneTo(glm::vec3 pos, glm::quat rot)
		{
			world.Get<TransformComponent>(bone).localToWorld = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot);
		}
	};

	void CheckMatEq(const glm::mat4& a, const glm::mat4& b, const char* what)
	{
		INFO(what);
		for (int col = 0; col < 4; ++col)
		{
			for (int row = 0; row < 4; ++row)
			{
				CHECK(a[col][row] == doctest::Approx(b[col][row]).epsilon(1e-4));
			}
		}
	}

	// "mixamorig:TestNode" deliberately sits at index 2, not 0, so a test that only
	// checked the transform (and not nodeIndex) could not pass against a function
	// that always wrote index 0.
	const std::vector<std::string> kNodeNames = {"mixamorig:Hips", "mixamorig:Spine", "mixamorig:TestNode", "mixamorig:LeftForeArm"};
} // namespace

TEST_CASE("At bind pose, a ragdoll bone's skin override reproduces the node's own bind-pose transform exactly")
{
	// Body and node bind poses are deliberately far apart (different position AND
	// rotation) - if the offset math secretly assumed they coincided, this would
	// already fail here, before any motion is involved.
	OneBoneRagdoll fx({0.1f, 1.5f, -0.2f}, glm::angleAxis(glm::radians(15.0f), glm::vec3(0.0f, 1.0f, 0.0f)), {2.0f, 0.3f, 5.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	const auto overrides = BuildRagdollSkinOverrides(fx.world, fx.root, kNodeNames, glm::mat4(1.0f));
	REQUIRE(overrides.size() == 1);
	CHECK(overrides[0].nodeIndex == 2);
	CheckMatEq(overrides[0].transform, fx.nodeBindWorld, "override at t=0 must equal the node's own bind pose");
}

TEST_CASE("After the body moves, the skin override tracks exactly the body's rigid displacement")
{
	// Body and node coincide at bind pose here (skinNodeOffset is identity), so the
	// expected result after moving the body is trivial to hand-check: the node ends up
	// exactly where the body moved to.
	OneBoneRagdoll fx({0.0f, 1.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {0.0f, 1.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	const glm::vec3 delta(3.0f, 0.0f, 0.0f);
	const glm::quat bodyRot = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
	fx.MoveBoneTo(glm::vec3(0.0f, 1.0f, 0.0f) + delta, bodyRot);

	const auto overrides = BuildRagdollSkinOverrides(fx.world, fx.root, kNodeNames, glm::mat4(1.0f));
	REQUIRE(overrides.size() == 1);
	const glm::mat4 expected = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f) + delta) * glm::mat4_cast(bodyRot);
	CheckMatEq(overrides[0].transform, expected, "override after motion must equal the body's new world transform");
}

TEST_CASE("The mesh's own world transform is divided out of the override - the result is in model space, not world space")
{
	OneBoneRagdoll fx({0.0f, 1.0f, 2.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {0.0f, 1.0f, 2.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

	// The mesh's own world transform carries BOTH a rotation and a translation, and
	// the node's own bind position (z=2) is deliberately off the rotation axis, so
	// applying meshWorldToModel in the wrong order (nodeWorld * meshWorldToModel
	// instead of meshWorldToModel * nodeWorld) changes the result. A translation-only
	// mesh transform, or a node position sitting ON the rotation axis, would not have
	// caught that mistake - both make the two orders coincide.
	const glm::mat4 meshWorld = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)) * glm::mat4_cast(glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
	const glm::mat4 meshWorldToModel = glm::inverse(meshWorld);
	const auto overrides = BuildRagdollSkinOverrides(fx.world, fx.root, kNodeNames, meshWorldToModel);
	REQUIRE(overrides.size() == 1);

	const glm::mat4 nodeWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 2.0f));
	const glm::mat4 expected = meshWorldToModel * nodeWorld;
	CheckMatEq(overrides[0].transform, expected, "override must be meshWorldToModel * nodeWorld, in that order");
}

TEST_CASE("A bone whose skinNodeName is not in the target node list is skipped, not crashed")
{
	OneBoneRagdoll fx({0.0f, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {0.0f, 0.0f, 0.0f}, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
	const std::vector<std::string> unrelatedNames = {"completely", "different", "skeleton"};
	const auto overrides = BuildRagdollSkinOverrides(fx.world, fx.root, unrelatedNames, glm::mat4(1.0f));
	CHECK(overrides.empty());
}

TEST_CASE("A ragdollRoot with no RagdollComponent yields no overrides, not a crash")
{
	World world;
	const Entity stray = world.Create();
	const auto overrides = BuildRagdollSkinOverrides(world, stray, kNodeNames, glm::mat4(1.0f));
	CHECK(overrides.empty());
}
