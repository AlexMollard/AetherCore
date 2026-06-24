#pragma once

#include "daScript/daScript.h"
#include "scene/World.hpp"

#include <string>
#include <vector>

#include "scripting/DasHelpers.hpp"

// -- World type factory --------------------------------------------------------
// Makes aether::World* usable as the 'World' type in daScript function signatures.
// WorldModule registers the matching DummyTypeAnnotation named "World".

MAKE_TYPE_FACTORY(World, aether::World)

// -- Auto-registration ----------------------------------------------------------
// Each module .cpp ends with AETHER_DAS_MODULE(ClassName, namespace), which
// self-registers via a static initialiser. ScriptingSubsystem::EnsureModulesRegistered
// just iterates GetModuleRegistrars() - no manual include or call needed there.
// Adding a new module requires zero changes outside its own .cpp file.

inline std::vector<void (*)()>& GetModuleRegistrars()
{
	static std::vector<void (*)()> v; // Meyers singleton - safe init order
	return v;
}

#define AETHER_DAS_MODULE(cls, ns)                                             \
	REGISTER_MODULE_IN_NAMESPACE(cls, ns);                                     \
	namespace                                                                  \
	{                                                                          \
		static bool _aether_reg_##cls = [] {                                   \
			GetModuleRegistrars().push_back([] { PULL_MODULE(cls); });         \
			return true;                                                       \
		}();                                                                   \
	}

// -- Generic component operation templates -------------------------------------
// Written once; instantiated per component type via BIND_COMPONENT.
// Scripts receive World as an explicit first argument, matching aether::World*.

template<typename C>
static void world_add_component(aether::World* w, uint32_t id)
{
	w->Emplace<C>(aether::Entity{id});
}

template<typename C>
static bool world_has_component(aether::World* w, uint32_t id)
{
	return w->Has<C>(aether::Entity{id});
}

template<typename C>
static void world_remove_component(aether::World* w, uint32_t id)
{
	w->Remove<C>(aether::Entity{id});
}

// -- DasModuleBase --------------------------------------------------------------
// Inherit from this instead of das::Module directly.

namespace aether::app::scripting
{
	class DasModuleBase : public das::Module
	{
	protected:
		using SE = das::SideEffects;

		explicit DasModuleBase(const char* name)
		      : das::Module(name)
		{
		}

		// Bind<fn>(lib, "das_name", SE::modifyExternal)
		// Replaces the verbose addExtern<DAS_BIND_FUN(fn)>(*this, lib, ...) form.
		// Uses decltype(Fn), Fn directly because DAS_BIND_FUN(a) expands to
		// decltype(&a), a - and &Fn is invalid when Fn is already a pointer NTTP.
		template<auto Fn>
		void Bind(das::ModuleLibrary& lib, const char* dasName, das::SideEffects fx)
		{
			addExtern<decltype(Fn), Fn>(*this, lib, dasName, fx, dasName);
		}
	};
} // namespace aether::app::scripting

// -- Generic per-component-type iteration ---------------------------------------
// for_each<Components...>(world, block)  ->  iterates entities via EnTT view
template<typename... Components>
static void for_each_components(aether::World* w, const das::TBlock<void, uint32_t>& block, das::Context* ctx, das::LineInfoArg* at)
{
	for (auto enttE: w->View<Components...>())
	{
		const auto id = static_cast<uint32_t>(entt::to_integral(enttE));
		vec4f args[1];
		args[0] = das::cast<uint32_t>::from(id);
		ctx->invoke(block, args, nullptr, at);
	}
}

// BIND_FOR_EACH("name", ComponentType...)  -- used inside DasModuleBase ctor
// Expands to a Bind<> call that registers for_each_name() in the library.
#define BIND_FOR_EACH(NAME, ...) \
    Bind<for_each_components<__VA_ARGS__>>(lib, "for_each_" NAME, SE::accessExternal);

// BIND_COMPONENT("name", ComponentType)
// Expands to three Bind<> calls inside a DasModuleBase constructor (lib must be in scope):
//   add_name(world, entity_id)    - emplaces the component
//   has_name(world, entity_id)    -> bool
//   remove_name(world, entity_id) - removes the component
#define BIND_COMPONENT(NAME, TYPE)                                                 \
	Bind<world_add_component<TYPE>>(lib, "add_" NAME, SE::modifyExternal);         \
	Bind<world_has_component<TYPE>>(lib, "has_" NAME, SE::accessExternal);         \
	Bind<world_remove_component<TYPE>>(lib, "remove_" NAME, SE::modifyExternal);

// -- Generic field accessor templates ------------------------------------------
// Cuts the per-component-field boilerplate from 2 hand-written wrappers + 2
// Bind calls down to one BIND_FIELD / BIND_STRING_FIELD invocation.  Derives
// the component and field types from the member pointer NTTP.

template<typename T>
struct das_member_traits;

template<typename C, typename T>
struct das_member_traits<T C::*>
{
	using class_type = C;
	using field_type = T;
};

// Scalar / value-type field get/set.
//   get_xxx(world, entity_id) -> field_type (returns default-constructed T{} when missing)
//   set_xxx(world, entity_id, value)
template<auto Member>
auto das_get_field(aether::World* w, uint32_t id) -> typename das_member_traits<decltype(Member)>::field_type
{
	using traits_t = das_member_traits<decltype(Member)>;
	using C = typename traits_t::class_type;
	using T = typename traits_t::field_type;
	auto c = w->TryGet<C>(aether::Entity{id});
	return c ? ((*c).*Member) : T{};
}

template<auto Member>
void das_set_field(aether::World* w, uint32_t id, typename das_member_traits<decltype(Member)>::field_type val)
{
	using traits_t = das_member_traits<decltype(Member)>;
	using C = typename traits_t::class_type;
	if (auto c = w->TryGet<C>(aether::Entity{id}))
	{
		(*c).*Member = val;
	}
}

// std::string field get/set with daScript heap ownership.
//   get_xxx(world, entity_id, ctx) -> char*   (deep-copied into daScript heap)
//   set_xxx(world, entity_id, value: const char*)
template<auto Member>
char* das_get_string_field(aether::World* w, uint32_t id, das::Context* ctx)
{
	using traits_t = das_member_traits<decltype(Member)>;
	using C = typename traits_t::class_type;
	auto c = w->TryGet<C>(aether::Entity{id});
	return c ? aether::app::scripting::das_string(ctx, (*c).*Member) : nullptr;
}

template<auto Member>
void das_set_string_field(aether::World* w, uint32_t id, const char* val)
{
	using traits_t = das_member_traits<decltype(Member)>;
	using C = typename traits_t::class_type;
	if (auto c = w->TryGet<C>(aether::Entity{id}))
	{
		(*c).*Member = aether::app::scripting::das_to_std_string(val);
	}
}

// BIND_FIELD(NAME, CLASS, MEMBER)  -- scalar/value field (float / uint / int / ...)
// Expands to two Bind<> calls for get_NAME / set_NAME.  lib must be in scope.
#define BIND_FIELD(NAME, CLASS, MEMBER)                                                \
	Bind<das_get_field<&CLASS::MEMBER>>(lib, "get_" NAME, SE::accessExternal);         \
	Bind<das_set_field<&CLASS::MEMBER>>(lib, "set_" NAME, SE::modifyExternal);

// BIND_STRING_FIELD(NAME, CLASS, MEMBER)  -- std::string field
// Field type must be std::string; get returns char* (das heap-allocated),
// set takes const char* (deep-copied into the component).
#define BIND_STRING_FIELD(NAME, CLASS, MEMBER)                                         \
	Bind<das_get_string_field<&CLASS::MEMBER>>(lib, "get_" NAME, SE::accessExternal); \
	Bind<das_set_string_field<&CLASS::MEMBER>>(lib, "set_" NAME, SE::modifyExternal);
