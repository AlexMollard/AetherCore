// ProjectKindForTemplate used to be a two-way ternary keyed on Blank2D alone
// ("== Blank2D ? Scene2D : Scene3D"), which silently mis-derives Scene3D for any
// OTHER 2D template - exactly the trap a third or fourth entry (Multiplayer2D,
// Multiplayer3D) would fall into if that ternary had simply grown another branch.
// It is now looked up from kProjectTemplates itself, so these tests check every
// entry agrees with its own declared kind rather than pinning that ternary's blind
// spot. Header-only: ProjectKindForTemplate and kProjectTemplates are both
// `inline constexpr` in ProjectCommon.hpp, so this needs no ProjectCommon.cpp link.
#include <doctest/doctest.h>

#include "project/ProjectCommon.hpp"

using aether::app::ProjectKind;
using aether::app::project::kProjectTemplates;
using aether::app::project::ProjectKindForTemplate;
using aether::app::project::ProjectTemplate;

TEST_CASE("ProjectKindForTemplate agrees with kProjectTemplates for every listed template")
{
	for (const auto& info: kProjectTemplates)
	{
		CHECK(ProjectKindForTemplate(info.value) == info.kind);
	}
}

TEST_CASE("ProjectKindForTemplate reports 2D for both the blank and multiplayer 2D templates")
{
	CHECK(ProjectKindForTemplate(ProjectTemplate::Blank2D) == ProjectKind::Scene2D);
	CHECK(ProjectKindForTemplate(ProjectTemplate::Multiplayer2D) == ProjectKind::Scene2D);
}

TEST_CASE("ProjectKindForTemplate reports 3D for both the blank and multiplayer 3D templates")
{
	CHECK(ProjectKindForTemplate(ProjectTemplate::Blank3D) == ProjectKind::Scene3D);
	CHECK(ProjectKindForTemplate(ProjectTemplate::Multiplayer3D) == ProjectKind::Scene3D);
}

TEST_CASE("kProjectTemplates lists exactly the four templates this batch shipped")
{
	CHECK(kProjectTemplates.size() == 4);
}
