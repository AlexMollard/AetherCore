// scene.get lists every component on an entity, naming the reflected ones by their friendly
// name. The rest arrived as whatever the compiler calls the type, so one list mixed
// "Rigid Body" with "struct aether::ScriptComponent" - and the raw spelling matches nothing
// a caller can pass back to the other scene methods.
#include <doctest/doctest.h>

#include <string>

#include "scene/reflection/Reflection.hpp"

using aether::reflect::PrettyComponentName;

TEST_CASE("A compiler type name becomes a readable component name")
{
	// What MSVC actually hands back through entt.
	CHECK(PrettyComponentName("struct aether::ScriptComponent") == "Script");
	CHECK(PrettyComponentName("struct aether::PhysicsStateComponent") == "Physics State");
	CHECK(PrettyComponentName("struct aether::SceneNodeComponent") == "Scene Node");
	// Clang and GCC omit the struct keyword.
	CHECK(PrettyComponentName("aether::HierarchyComponent") == "Hierarchy");
	CHECK(PrettyComponentName("class aether::FooComponent") == "Foo");
}

TEST_CASE("Names that do not fit the pattern are left alone")
{
	// A tag has no Component suffix to strip.
	CHECK(PrettyComponentName("struct aether::HiddenTag") == "Hidden Tag");
	// Nothing but the suffix would leave an empty name, which is worse than the raw one.
	CHECK(PrettyComponentName("struct aether::Component") == "struct aether::Component");
	CHECK(PrettyComponentName("") == "");
}
