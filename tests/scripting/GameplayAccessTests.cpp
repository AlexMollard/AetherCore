#include <doctest/doctest.h>

#include <thread>

#include "scene/World.hpp"
#include "scripting/GameplayAccess.hpp"

using namespace aether;
using namespace aether::scripting;

TEST_CASE("Gameplay access rejects wrong world and destroyed generation handles")
{
	World world;
	GameplayAccess access(world);
	GameplayAccess other(world);
	const auto entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	const auto handle = access.Handle(entity);
	const auto api = access.EngineApi();
	uint32_t valid = 99;
	CHECK(api.entityValid(api.context, handle, &valid) == AE_GAMEPLAY_OK);
	CHECK(valid == 1);
	CHECK(api.entityValid(api.context, other.Handle(entity), &valid) == AE_GAMEPLAY_WRONG_WORLD);
	CHECK(valid == 0);
	world.Destroy(entity);
	const auto recycled = world.Create();
	CHECK(api.entityValid(api.context, handle, &valid) == AE_GAMEPLAY_INVALID_HANDLE);
	CHECK(valid == 0);
	CHECK(recycled.id != entity.id);
}

TEST_CASE("Gameplay access copies transforms and rejects stale contexts and wrong threads")
{
	World world;
	GameplayAccess access(world);
	const auto entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	const auto handle = access.Handle(entity);
	const auto api = access.EngineApi();
	CHECK(api.setPosition(api.context, handle, {2, 3, 4}) == AE_GAMEPLAY_OK);
	AetherGameplayVec3 position{};
	CHECK(api.getPosition(api.context, handle, &position) == AE_GAMEPLAY_OK);
	CHECK(position.x == 2);
	AetherGameplayStatus status = AE_GAMEPLAY_OK;
	std::thread other([&]() { status = api.getPosition(api.context, handle, &position); });
	other.join();
	CHECK(status == AE_GAMEPLAY_WRONG_THREAD);
	access.Invalidate();
	CHECK(api.getPosition(api.context, handle, &position) == AE_GAMEPLAY_INVALID_HANDLE);
	CHECK(position.x == 0);
}

TEST_CASE("Gameplay callback and structural mutation scopes reject reentrancy")
{
	World world;
	GameplayAccess access(world);
	{
		GameplayAccess::Scope callback(access, GameplayAccess::ScopeKind::Callback);
		REQUIRE(callback.Status() == AE_GAMEPLAY_OK);
		GameplayAccess::Scope nested(access, GameplayAccess::ScopeKind::Callback);
		CHECK(nested.Status() == AE_GAMEPLAY_REENTRANT);
		GameplayAccess::Scope mutation(access, GameplayAccess::ScopeKind::StructuralMutation);
		CHECK(mutation.Status() == AE_GAMEPLAY_REENTRANT);
		GameplayAccess::Scope predicted(access, GameplayAccess::ScopeKind::PredictedAttachCallback);
		CHECK(predicted.Status() == AE_GAMEPLAY_REENTRANT);
#if defined(__EMSCRIPTEN__)
		{
			GameplayAccess::PredictedAttachScope permission(access);
			GameplayAccess::Scope create(access, GameplayAccess::ScopeKind::PredictedAttachCallback);
			CHECK(create.Status() == AE_GAMEPLAY_OK);
			GameplayAccess::Scope update(access, GameplayAccess::ScopeKind::Callback);
			CHECK(update.Status() == AE_GAMEPLAY_REENTRANT);
			GameplayAccess::Scope structural(access, GameplayAccess::ScopeKind::StructuralMutation);
			CHECK(structural.Status() == AE_GAMEPLAY_REENTRANT);
		}
		GameplayAccess::Scope after(access, GameplayAccess::ScopeKind::PredictedAttachCallback);
		CHECK(after.Status() == AE_GAMEPLAY_REENTRANT);
#endif
	}
	GameplayAccess::Scope mutation(access, GameplayAccess::ScopeKind::StructuralMutation);
	CHECK(mutation.Status() == AE_GAMEPLAY_OK);
	GameplayAccess::Scope callback(access, GameplayAccess::ScopeKind::Callback);
	CHECK(callback.Status() == AE_GAMEPLAY_REENTRANT);
}

TEST_CASE("Gameplay diagnostics validate text and contain errors without crossing the ABI")
{
	World world;
	uint32_t calls = 0;
	GameplayAccess access(
	        world,
	        [](void* user, const AetherGameplayDiagnostic&) -> int32_t
	        {
		        ++*static_cast<uint32_t*>(user);
		        return AE_GAMEPLAY_SCRIPT_ERROR;
	        },
	        &calls);
	const auto api = access.EngineApi();
	AetherGameplayDiagnostic diagnostic{};
	diagnostic.structSize = sizeof(diagnostic);
	diagnostic.severity = AE_GAMEPLAY_DIAGNOSTIC_ERROR;
	diagnostic.code = {"script.failed", 13};
	diagnostic.message = {"\xE7\x8E\xA9\xE5\xAE\xB6", 6};
	CHECK(api.reportDiagnostic(api.context, &diagnostic) == AE_GAMEPLAY_SCRIPT_ERROR);
	CHECK(calls == 1);
	diagnostic.severity = 3;
	CHECK(api.reportDiagnostic(api.context, &diagnostic) == AE_GAMEPLAY_INVALID_ARGUMENT);
	diagnostic.severity = 2;
	diagnostic.code.length = 0;
	CHECK(api.reportDiagnostic(api.context, &diagnostic) == AE_GAMEPLAY_INVALID_ARGUMENT);
	diagnostic.code = {"x", 1};
	diagnostic.message = {"\xED\xA0\x80", 3};
	CHECK(api.reportDiagnostic(api.context, &diagnostic) == AE_GAMEPLAY_INVALID_ARGUMENT);
	CHECK(calls == 1);
}
