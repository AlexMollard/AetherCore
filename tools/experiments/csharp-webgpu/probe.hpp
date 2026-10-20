#pragma once

#include "../../../src/engine/scripting/GameplayAbi.h"

extern "C"
{
	int aether_probe_scalar(int value);
	int aether_probe_utf8(const unsigned char* value, unsigned length);
	void aether_probe_vec3(AetherGameplayVec3 value, AetherGameplayVec3* result);
	AetherGameplayVec3 aether_probe_return_vec3();
	int aether_probe_ticks(int (*callback)(int), int ticks);
}
