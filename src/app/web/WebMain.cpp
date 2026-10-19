#include "WebMain.hpp"

#include <box2d/box2d.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "PakFormat.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayAccess.hpp"

int main()
{
	// Runtime foundation: execute the engine's actual ECS/ABI and pinned Box2D.
	// Browser GPU/audio/input/storage lifecycle is deliberately not ready yet.
	aether::World world;
	const auto entity = world.Create();
	world.Emplace<aether::TransformComponent>(entity);
	aether::scripting::GameplayAccess access(world);
	aether::scripting::GameplayAccess::Scope scope(access, aether::scripting::GameplayAccess::ScopeKind::Callback);
	const auto api = access.EngineApi();
	const auto handle = access.Handle(entity);
	AetherGameplayVec3 desired{2.0f, 3.0f, 0.0f};
	const auto moved = api.setPosition(api.context, handle, desired);

	b2WorldDef definition = b2DefaultWorldDef();
	definition.workerCount = 0;
	definition.gravity = {0.0f, -10.0f};
	const b2WorldId physics = b2CreateWorld(&definition);
	b2BodyDef bodyDefinition = b2DefaultBodyDef();
	bodyDefinition.type = b2_dynamicBody;
	const b2BodyId body = b2CreateBody(physics, &bodyDefinition);
	b2ShapeDef shapeDefinition = b2DefaultShapeDef();
	shapeDefinition.density = 1.0f;
	const b2Circle circle{{0.0f, 0.0f}, 0.5f};
	b2CreateCircleShape(body, &shapeDefinition, &circle);
	for (int tick = 0; tick < 60; ++tick)
	{
		b2World_Step(physics, 1.0f / 60.0f, 4);
	}
	const float y = b2Body_GetPosition(body).y;
	b2DestroyWorld(physics);
	std::ifstream package("/assets/reference.pak", std::ios::binary);
	PakHeader header;
	package.read(reinterpret_cast<char*>(&header), sizeof(header));
	std::ifstream shader("/assets/sprite.wgsl");
	const bool passed = moved == AE_GAMEPLAY_OK && world.TryGet<aether::TransformComponent>(entity)->localToWorld[3].x == 2.0f && y < -4.0f && package.good() && std::memcmp(header.magic, "AEPK", 4) == 0 && header.version == PAK_VERSION
	                    && header.numEntries > 0 && shader.good();
	std::printf("{\"profile\":\"browser-foundation-v1\",\"status\":\"%s\",\"worldEntity\":%u,\"physicsY\":%f,\"workers\":0,\"rendererReady\":false,\"audioReady\":false,\"storageReady\":false,\"inputReady\":false}\n",
	        passed ? "passed" : "failed",
	        entity.id,
	        y);
	return passed ? 0 : 1;
}
