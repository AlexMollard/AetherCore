// Script joint offsets (Animation.SetJointOffset): the CPU half of turning one animated node on
// top of its sampled pose. node_flatten.slang composes each node as parent * local and, for a
// kNodeOverrideLocalRotation entry, post-multiplies the entry's transform onto the local, so
// these cases compose a synthetic chain exactly that way from the packed entries.
#include <doctest/doctest.h>

#include <string>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "rendering/RagdollSkinDrive.hpp"
#include "scene/Components.hpp"

using namespace aether;

TEST_CASE("A 90 degree joint offset swings the child joint round with it")
{
	// Parent at the origin, child one unit down +Z; the offset turns the parent 90 degrees about +Y.
	const glm::mat4 parentLocal{1.0f};
	const glm::mat4 childLocal = glm::translate(glm::mat4{1.0f}, glm::vec3(0.0f, 0.0f, 1.0f));
	const std::vector<JointRotationOffset> offsets{{.node = 0, .rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))}};

	std::vector<AnimationContracts::RagdollOverrideEntry> entries;
	AppendJointRotationOverrides(offsets, entries);
	REQUIRE(entries.size() == 1);
	CHECK(entries[0].nodeIndex == 0);
	CHECK(entries[0].kind == AnimationContracts::kNodeOverrideLocalRotation);

	const glm::mat4 parentGlobal = parentLocal * entries[0].transform;
	const glm::vec3 child = glm::vec3((parentGlobal * childLocal)[3]);
	CHECK(child.x == doctest::Approx(1.0f).epsilon(1e-5));
	CHECK(child.y == doctest::Approx(0.0f).epsilon(1e-5));
	CHECK(child.z == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("Joint offsets resolve by exact or prefix-free node name, case-insensitively")
{
	const std::vector<std::string> names{"joint0", "Armature_joint1", "joint14"};
	CHECK(FindNodeIndexByName(names, "JOINT14") == 2u);
	CHECK(FindNodeIndexByName(names, "joint1") == 1u);
	CHECK_FALSE(FindNodeIndexByName(names, "joint99").has_value());
}
